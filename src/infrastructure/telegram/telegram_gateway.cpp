// ---------------------------------------------------------------------------
// telegram_gateway.cpp
//
// Sole translation unit in CMLB that includes the TDLib headers. Everything
// here is private to the gateway's PIMPL — neither types nor functions are
// re-exported.
//
// Threading model:
//   * One `boost::asio::strand` (`strand_`) owns the TDLib client.
//   * Every public method first `co_await`s `dispatch(strand_)` so the actual
//     TDLib `send` happens on the strand.
//   * `run()` reads from TDLib via `client_manager_->receive()` on the strand
//     in a loop, then dispatches.
//
// Pending request lifecycle:
//   * `register_request` allocates the next id from `next_request_id_` (starts
//     at 1000), stores a `PendingHandler` with `created_at = steady_clock::now()`
//     and the awaiter, and returns the id.
//   * `handle_response` looks up the handler, removes it, and invokes it.
//   * `evict_stale()` runs every `kEvictionInterval` and completes handlers
//     whose age exceeds the configured TTL with `ErrorCode::Timeout`.
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <fmt/format.h>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

namespace cmlb::infrastructure::telegram {

namespace asio = boost::asio;
namespace td_api = td::td_api;

namespace {

/// Convert an HTML snippet into a `formattedText`. On parse failure, fall
/// back to plain text containing the literal HTML so the message still goes
/// out (rather than failing the whole send).
[[nodiscard]] td_api::object_ptr<td_api::formattedText> parse_html(const std::string& html) {
    auto parse_request = td_api::make_object<td_api::parseTextEntities>(
        html, td_api::make_object<td_api::textParseModeHTML>());
    auto parsed = td::ClientManager::execute(std::move(parse_request));
    if (parsed && parsed->get_id() == td_api::formattedText::ID) {
        return td_api::move_object_as<td_api::formattedText>(parsed);
    }
    core::Logger::warn("telegram_gateway: HTML parse failed, falling back to plain text");
    return td_api::make_object<td_api::formattedText>(
        html, std::vector<td_api::object_ptr<td_api::textEntity>>{});
}

/// Translate the gateway's raw `(label, callback_data)` rows into a TDLib
/// inline keyboard.
[[nodiscard]] td_api::object_ptr<td_api::replyMarkupInlineKeyboard> build_inline_keyboard(
    const std::vector<std::vector<std::pair<std::string, std::string>>>& rows) {
    std::vector<std::vector<td_api::object_ptr<td_api::inlineKeyboardButton>>> kb_rows;
    kb_rows.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<td_api::object_ptr<td_api::inlineKeyboardButton>> kb_row;
        kb_row.reserve(row.size());
        for (const auto& [label, data] : row) {
            // TDLib's `bytes` alias is `std::string` — pass payload directly.
            auto button_type = td_api::make_object<td_api::inlineKeyboardButtonTypeCallback>(data);
            kb_row.push_back(td_api::make_object<td_api::inlineKeyboardButton>(
                label, td_api::move_object_as<td_api::InlineKeyboardButtonType>(button_type)));
        }
        kb_rows.push_back(std::move(kb_row));
    }
    return td_api::make_object<td_api::replyMarkupInlineKeyboard>(std::move(kb_rows));
}

/// Extract a `std::string` from a TDLib byte payload. `td_api::bytes` is a
/// `std::string` alias in current TDLib, so this is effectively identity —
/// kept as a typed seam in case TDLib renames the alias again.
[[nodiscard]] std::string bytes_to_string(const std::string& bytes) {
    return bytes;
}

/// Map a TDLib `error` object onto a CMLB `AppError`.
[[nodiscard]] core::AppError td_error_to_app_error(const td_api::error& err,
                                                   std::string_view operation) {
    return core::AppError{
        core::ErrorCode::TelegramApi,
        fmt::format("telegram {}: code={} message={}", operation, err.code_, err.message_)};
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct TelegramGateway::Impl {
    /// One slot in the pending-request map.
    struct PendingHandler {
        std::function<void(td_api::object_ptr<td_api::Object>)> resume;
        std::chrono::steady_clock::time_point created_at;
    };

    Impl(core::Executor& exec, core::TelegramConfig cfg)
        : executor{exec},
          config{std::move(cfg)},
          strand{asio::make_strand(exec.get_executor())},
          eviction_timer{strand},
          request_ttl{TelegramGateway::kDefaultRequestTtl} {
    }

    core::Executor& executor;
    core::TelegramConfig config;
    asio::strand<asio::any_io_executor> strand;
    asio::steady_timer eviction_timer;

    /// The PIMPL owns the TDLib client manager. The client id is allocated by
    /// `create_client_id()` during `run()`'s bring-up.
    std::unique_ptr<td::ClientManager> client_manager;
    std::int32_t client_id{0};

    /// Background thread that blocks in `client_manager->receive(timeout)` and
    /// posts events onto `strand` as they arrive. Started by `run()`, joined
    /// before `run()` returns. Lets TDLib wake on its own timeline instead of
    /// the strand polling every 100 ms.
    std::thread receive_thread;

    /// Request ids start at 1000 so they cannot collide with the reserved
    /// internal ids (1, 2, 3, 100) that older code paths used.
    std::atomic<std::uint64_t> next_request_id{1000};

    /// All accesses to `pending` happen on `strand`.
    std::unordered_map<std::uint64_t, PendingHandler> pending;

    /// TTL for pending handlers. Sweeps run every `kEvictionInterval`.
    std::chrono::seconds request_ttl;

    /// User-facing update handlers. Mutated only on `strand`.
    UpdateHandlers handlers;

    /// Internal auth-state listener. Mutated only on `strand`.
    AuthStateHandler auth_handler;

    /// Most recent auth state observed from TDLib. Set on `strand` whenever
    /// a `updateAuthorizationState` arrives. Replayed to a fresh handler at
    /// `set_auth_state_handler` time so a slow listener install can't lose
    /// the early `WaitTdlibParameters` transition.
    std::optional<TelegramGateway::AuthState> last_auth_state;

    /// Flag set by `request_stop`. Polled by `run()`.
    std::atomic<bool> stop_requested{false};

    /// Allocates a fresh request id.
    [[nodiscard]] std::uint64_t allocate_id() noexcept {
        return next_request_id.fetch_add(1, std::memory_order_relaxed);
    }

    /// Registers a pending handler. Must be called on `strand`.
    void register_handler(std::uint64_t id,
                          std::function<void(td_api::object_ptr<td_api::Object>)> resume) {
        pending.emplace(id, PendingHandler{std::move(resume), std::chrono::steady_clock::now()});
    }

    /// Completes any pending handlers older than `request_ttl` with a
    /// synthetic timeout error.
    void evict_stale() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->second.created_at >= request_ttl) {
                auto err = td_api::make_object<td_api::error>(
                    /*code=*/408,
                    /*message=*/std::string{"cmlb: request timed out client-side"});
                auto resume = std::move(it->second.resume);
                it = pending.erase(it);
                // Invoke after erasing so handlers can't reentrantly mutate
                // the map under us.
                resume(td_api::move_object_as<td_api::Object>(err));
            } else {
                ++it;
            }
        }
    }

    /// Schedules the next eviction sweep.
    void arm_eviction_timer() {
        eviction_timer.expires_after(TelegramGateway::kEvictionInterval);
        eviction_timer.async_wait([this](const boost::system::error_code& ec) {
            if (ec) {
                return; // cancelled — gateway shutting down.
            }
            evict_stale();
            if (!stop_requested.load(std::memory_order_acquire)) {
                arm_eviction_timer();
            }
        });
    }

    /// Dispatches a single TDLib response object delivered by `receive()`.
    /// Runs on `strand`.
    void handle_event(td::ClientManager::Response response) {
        if (!response.object) {
            return;
        }
        if (response.request_id == 0) {
            // Update (unsolicited message from TDLib).
            dispatch_update(std::move(response.object));
            return;
        }
        auto it = pending.find(response.request_id);
        if (it == pending.end()) {
            // Likely already evicted by the TTL sweep — discard.
            return;
        }
        auto resume = std::move(it->second.resume);
        pending.erase(it);
        resume(std::move(response.object));
    }

    /// Routes an `updateXxx` object to the user-installed handlers.
    void dispatch_update(td_api::object_ptr<td_api::Object> obj) {
        switch (obj->get_id()) {
        case td_api::updateNewMessage::ID:
            dispatch_new_message(td_api::move_object_as<td_api::updateNewMessage>(obj));
            break;
        case td_api::updateNewCallbackQuery::ID:
            dispatch_callback_query(td_api::move_object_as<td_api::updateNewCallbackQuery>(obj));
            break;
        case td_api::updateFile::ID:
            dispatch_file_update(td_api::move_object_as<td_api::updateFile>(obj));
            break;
        case td_api::updateAuthorizationState::ID:
            dispatch_auth_state(td_api::move_object_as<td_api::updateAuthorizationState>(obj));
            break;
        default:
            break; // ignore everything else for now
        }
    }

    void dispatch_auth_state(td_api::object_ptr<td_api::updateAuthorizationState> upd) {
        if (!upd || !upd->authorization_state_) {
            return;
        }
        AuthState state = AuthState::Other;
        switch (upd->authorization_state_->get_id()) {
        case td_api::authorizationStateWaitTdlibParameters::ID:
            state = AuthState::WaitTdlibParameters;
            break;
        case td_api::authorizationStateWaitPhoneNumber::ID:
            state = AuthState::WaitPhoneOrBotToken;
            break;
        case td_api::authorizationStateReady::ID:
            state = AuthState::Ready;
            break;
        case td_api::authorizationStateLoggingOut::ID:
            state = AuthState::LoggingOut;
            break;
        case td_api::authorizationStateClosing::ID:
            state = AuthState::Closing;
            break;
        case td_api::authorizationStateClosed::ID:
            state = AuthState::Closed;
            break;
        default:
            break;
        }
        last_auth_state = state;
        if (auth_handler) {
            auth_handler(state);
        }
    }

    void dispatch_new_message(td_api::object_ptr<td_api::updateNewMessage> upd) {
        if (!handlers.on_new_message || !upd || !upd->message_) {
            return;
        }
        auto& msg = *upd->message_;
        if (!msg.content_ || msg.content_->get_id() != td_api::messageText::ID) {
            return;
        }
        const auto& text_content = static_cast<const td_api::messageText&>(*msg.content_);
        std::string text;
        if (text_content.text_) {
            text = text_content.text_->text_;
        }
        std::int64_t sender_user_id = 0;
        if (msg.sender_id_ && msg.sender_id_->get_id() == td_api::messageSenderUser::ID) {
            sender_user_id =
                static_cast<const td_api::messageSenderUser&>(*msg.sender_id_).user_id_;
        }
        handlers.on_new_message(domain::ChatId{msg.chat_id_},
                                domain::UserId{sender_user_id},
                                domain::MessageId{msg.id_},
                                std::move(text));
    }

    void dispatch_callback_query(td_api::object_ptr<td_api::updateNewCallbackQuery> upd) {
        if (!handlers.on_callback_query || !upd) {
            return;
        }
        std::string data;
        if (upd->payload_ && upd->payload_->get_id() == td_api::callbackQueryPayloadData::ID) {
            const auto& payload =
                static_cast<const td_api::callbackQueryPayloadData&>(*upd->payload_);
            data = bytes_to_string(payload.data_);
        }
        handlers.on_callback_query(domain::ChatId{upd->chat_id_},
                                   domain::UserId{upd->sender_user_id_},
                                   domain::MessageId{upd->message_id_},
                                   domain::CallbackQueryId{upd->id_},
                                   std::move(data));
    }

    void dispatch_file_update(td_api::object_ptr<td_api::updateFile> upd) {
        if (!handlers.on_file_update || !upd || !upd->file_) {
            return;
        }
        const auto& file = *upd->file_;
        std::int64_t total = file.size_;
        std::int64_t downloaded = file.local_ ? file.local_->downloaded_size_ : 0;
        std::int64_t uploaded = file.remote_ ? file.remote_->uploaded_size_ : 0;
        bool ul_active = file.remote_ && file.remote_->is_uploading_active_;
        bool dl_active = file.local_ && file.local_->is_downloading_active_;
        bool ul_done = file.remote_ && file.remote_->is_uploading_completed_;
        bool dl_done = file.local_ && file.local_->is_downloading_completed_;
        handlers.on_file_update(domain::FileId{file.id_},
                                total,
                                downloaded,
                                uploaded,
                                ul_active,
                                dl_active,
                                ul_done,
                                dl_done);
    }

    /// Issues a TDLib request. Must run on `strand`. Returns the assigned id.
    std::uint64_t send_request(td_api::object_ptr<td_api::Function> fn,
                               std::function<void(td_api::object_ptr<td_api::Object>)> resume) {
        const auto id = allocate_id();
        register_handler(id, std::move(resume));
        client_manager->send(client_id, id, std::move(fn));
        return id;
    }
};

// ---------------------------------------------------------------------------
// TelegramGateway public methods
// ---------------------------------------------------------------------------

TelegramGateway::TelegramGateway(core::Executor& executor, core::TelegramConfig config)
    : impl_{std::make_unique<Impl>(executor, std::move(config))} {
}

TelegramGateway::~TelegramGateway() {
    request_stop();
    // Defensive: join the receive thread if `run()` did not get to it (e.g.
    // an early exception propagated out of the coroutine). Without this, the
    // jthread-equivalent join would happen during member destruction, after
    // `client_manager` is gone — which the receive thread is still touching.
    if (impl_ && impl_->receive_thread.joinable()) {
        impl_->receive_thread.join();
    }
}

void TelegramGateway::request_stop() noexcept {
    if (!impl_) {
        return;
    }
    impl_->stop_requested.store(true, std::memory_order_release);
    // Modern Asio dropped `cancel(error_code&)`; the no-arg overload is noexcept.
    impl_->eviction_timer.cancel();
}

void TelegramGateway::set_update_handlers(UpdateHandlers handlers) {
    asio::post(impl_->strand, [impl = impl_.get(), handlers = std::move(handlers)]() mutable {
        impl->handlers = std::move(handlers);
    });
}

void TelegramGateway::set_auth_state_handler(AuthStateHandler handler) {
    asio::post(impl_->strand, [impl = impl_.get(), handler = std::move(handler)]() mutable {
        impl->auth_handler = std::move(handler);
        // Replay the most recent observed state so a listener
        // that installs after the first TDLib update doesn't miss
        // the early `WaitTdlibParameters` transition.
        if (impl->auth_handler && impl->last_auth_state) {
            impl->auth_handler(*impl->last_auth_state);
        }
    });
}

// ---- run() ---------------------------------------------------------------

asio::awaitable<core::Result<void>> TelegramGateway::run() {
    auto* impl = impl_.get();
    co_await asio::dispatch(impl->strand, asio::use_awaitable);

    // Lazy-create the client manager + client id on the strand.
    if (!impl->client_manager) {
        // Quiet TDLib's own logger; default level 5 spams the receive loop.
        // Level 1 keeps fatal/error/warning, drops debug poll-tick noise.
        td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(1));
        impl->client_manager = std::make_unique<td::ClientManager>();
        impl->client_id = impl->client_manager->create_client_id();
        // Kick the client by sending a no-op so TDLib starts emitting
        // `updateAuthorizationState`.
        impl->client_manager->send(
            impl->client_id, /*request_id=*/0, td_api::make_object<td_api::getOption>("version"));
    }

    impl->arm_eviction_timer();

    // Spawn the dedicated TDLib receive thread. It blocks in `receive()` with
    // a moderate timeout (500 ms) so `stop_requested` is observed quickly on
    // shutdown without burning CPU when the network is idle. Events are
    // posted to the strand for handling so all `pending`/`handlers` mutations
    // remain strand-confined.
    if (!impl->receive_thread.joinable()) {
        impl->receive_thread = std::thread([impl] {
            constexpr double kReceiveTimeoutSec = 0.5;
            while (!impl->stop_requested.load(std::memory_order_acquire)) {
                auto response = impl->client_manager->receive(kReceiveTimeoutSec);
                if (!response.object) {
                    continue; // timeout — re-check stop flag.
                }
                asio::post(impl->strand, [impl, resp = std::move(response)]() mutable {
                    impl->handle_event(std::move(resp));
                });
            }
        });
    }

    core::Logger::info("telegram_gateway: receive loop started");

    // Park the strand-side coroutine until stop. The eviction timer keeps
    // ticking under us; the receive thread feeds events into the strand.
    while (!impl->stop_requested.load(std::memory_order_acquire)) {
        asio::steady_timer t{impl->strand};
        t.expires_after(std::chrono::seconds{1});
        boost::system::error_code ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            break; // cancelled
        }
    }

    // Join the receive thread before draining pending handlers so we cannot
    // race a late `handle_event` post against `pending.clear()`.
    if (impl->receive_thread.joinable()) {
        impl->receive_thread.join();
    }

    core::Logger::info("telegram_gateway: receive loop exited");

    // Drain pending handlers with a cancellation error.
    for (auto& [id, handler] : impl->pending) {
        auto err = td_api::make_object<td_api::error>(
            /*code=*/499, std::string{"cmlb: client stopped"});
        handler.resume(td_api::move_object_as<td_api::Object>(err));
    }
    impl->pending.clear();

    co_return core::Result<void>{};
}

// ---------------------------------------------------------------------------
// Request helpers
//
// `await_send` issues a TDLib request on the strand and `co_await`s the
// matching response via a `steady_timer` parked on `strand` that is cancelled
// when the response arrives. We use an asio `async_initiate`-style pattern
// implemented with a `boost::asio::experimental::channel` would be ideal, but
// channels require a dependency we have not yet pulled in. Instead we use a
// simple promise/future-style: the awaiter sets a captured pointer to the
// result and cancels the timer.
// ---------------------------------------------------------------------------

namespace {

/// Awaitable utility — sends `fn` on the strand and yields the matching
/// response object. The caller owns interpreting the response (success vs
/// `error` object).
asio::awaitable<td_api::object_ptr<td_api::Object>> await_send(
    TelegramGateway::Impl& impl, td_api::object_ptr<td_api::Function> fn) {
    co_await asio::dispatch(impl.strand, asio::use_awaitable);

    // The shared state lives on the heap so the lambda can outlive this
    // coroutine frame in the case of a TTL eviction.
    struct State {
        td_api::object_ptr<td_api::Object> result;
        asio::steady_timer timer;
        bool done{false};

        explicit State(asio::strand<asio::any_io_executor> s) : timer{s} {
        }
    };

    auto state = std::make_shared<State>(impl.strand);
    state->timer.expires_at(std::chrono::steady_clock::time_point::max());

    impl.send_request(std::move(fn), [state](td_api::object_ptr<td_api::Object> obj) {
        state->result = std::move(obj);
        state->done = true;
        state->timer.cancel();
    });

    boost::system::error_code ec;
    co_await state->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    // We expect ec == operation_aborted when the handler cancels the timer.
    // If the timer somehow fires without cancellation, treat it as a TDLib
    // error — but this should never happen since expires_at == max.
    if (!state->done) {
        state->result = td_api::make_object<td_api::error>(
            /*code=*/500, std::string{"cmlb: internal: timer fired before response"});
    }
    co_return std::move(state->result);
}

/// Interpret a TDLib response as `Result<MessageId>`. Expects a
/// `td_api::message` on success.
[[nodiscard]] core::Result<domain::MessageId> response_to_message_id(
    td_api::object_ptr<td_api::Object> obj, std::string_view operation) {
    if (!obj) {
        return core::error(core::ErrorCode::TelegramApi,
                           fmt::format("telegram {}: null response", operation));
    }
    if (obj->get_id() == td_api::error::ID) {
        return std::unexpected(
            td_error_to_app_error(static_cast<const td_api::error&>(*obj), operation));
    }
    if (obj->get_id() != td_api::message::ID) {
        return core::error(core::ErrorCode::TelegramApi,
                           fmt::format("telegram {}: unexpected response type", operation));
    }
    const auto& msg = static_cast<const td_api::message&>(*obj);
    return domain::MessageId{msg.id_};
}

/// Interpret a TDLib response as `Result<void>`. Accepts `ok` or any
/// non-error object as success.
[[nodiscard]] core::Result<void> response_to_void(td_api::object_ptr<td_api::Object> obj,
                                                  std::string_view operation) {
    if (!obj) {
        return core::error(core::ErrorCode::TelegramApi,
                           fmt::format("telegram {}: null response", operation));
    }
    if (obj->get_id() == td_api::error::ID) {
        return std::unexpected(
            td_error_to_app_error(static_cast<const td_api::error&>(*obj), operation));
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Typed sends
// ---------------------------------------------------------------------------

asio::awaitable<core::Result<domain::MessageId>> TelegramGateway::send_text_message(
    domain::ChatId chat, std::string text) {
    auto formatted = td_api::make_object<td_api::formattedText>(
        std::move(text), std::vector<td_api::object_ptr<td_api::textEntity>>{});
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(formatted),
                                                                 /*link_preview_options=*/nullptr,
                                                                 /*clear_draft=*/false);
    auto fn = td_api::make_object<td_api::sendMessage>(chat.value(),
                                                       /*topic_id=*/nullptr,
                                                       /*reply_to=*/nullptr,
                                                       /*options=*/nullptr,
                                                       /*reply_markup=*/nullptr,
                                                       std::move(content));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_message_id(std::move(obj), "send_text_message");
}

asio::awaitable<core::Result<domain::MessageId>> TelegramGateway::send_formatted_message(
    domain::ChatId chat, std::string html) {
    auto formatted = parse_html(html);
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(formatted),
                                                                 /*link_preview_options=*/nullptr,
                                                                 /*clear_draft=*/false);
    auto fn = td_api::make_object<td_api::sendMessage>(chat.value(),
                                                       /*topic_id=*/nullptr,
                                                       /*reply_to=*/nullptr,
                                                       /*options=*/nullptr,
                                                       /*reply_markup=*/nullptr,
                                                       std::move(content));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_message_id(std::move(obj), "send_formatted_message");
}

asio::awaitable<core::Result<void>> TelegramGateway::edit_formatted_message(domain::ChatId chat,
                                                                            domain::MessageId msg,
                                                                            std::string html) {
    auto formatted = parse_html(html);
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(formatted),
                                                                 /*link_preview_options=*/nullptr,
                                                                 /*clear_draft=*/false);
    auto fn = td_api::make_object<td_api::editMessageText>(chat.value(),
                                                           msg.value(),
                                                           /*reply_markup=*/nullptr,
                                                           std::move(content));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "edit_formatted_message");
}

asio::awaitable<core::Result<domain::MessageId>> TelegramGateway::send_message_with_inline_keyboard(
    domain::ChatId chat,
    std::string html,
    std::vector<std::vector<std::pair<std::string, std::string>>> rows) {
    auto formatted = parse_html(html);
    auto content = td_api::make_object<td_api::inputMessageText>(std::move(formatted),
                                                                 /*link_preview_options=*/nullptr,
                                                                 /*clear_draft=*/false);
    auto markup = build_inline_keyboard(rows);
    auto fn = td_api::make_object<td_api::sendMessage>(chat.value(),
                                                       /*topic_id=*/nullptr,
                                                       /*reply_to=*/nullptr,
                                                       /*options=*/nullptr,
                                                       std::move(markup),
                                                       std::move(content));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_message_id(std::move(obj), "send_message_with_inline_keyboard");
}

asio::awaitable<core::Result<void>> TelegramGateway::edit_message_inline_keyboard(
    domain::ChatId chat,
    domain::MessageId msg,
    std::vector<std::vector<std::pair<std::string, std::string>>> rows) {
    auto markup = build_inline_keyboard(rows);
    auto fn = td_api::make_object<td_api::editMessageReplyMarkup>(
        chat.value(), msg.value(), std::move(markup));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "edit_message_inline_keyboard");
}

asio::awaitable<core::Result<void>> TelegramGateway::answer_callback_query(
    domain::CallbackQueryId id, std::string text, bool show_alert) {
    auto fn = td_api::make_object<td_api::answerCallbackQuery>(id.value(),
                                                               std::move(text),
                                                               show_alert,
                                                               /*url=*/std::string{},
                                                               /*cache_time=*/0);
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "answer_callback_query");
}

asio::awaitable<core::Result<void>> TelegramGateway::delete_message(domain::ChatId chat,
                                                                    domain::MessageId msg) {
    std::vector<std::int64_t> ids{msg.value()};
    auto fn =
        td_api::make_object<td_api::deleteMessages>(chat.value(), std::move(ids), /*revoke=*/true);
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "delete_message");
}

asio::awaitable<core::Result<void>> TelegramGateway::set_tdlib_parameters() {
    auto fn = td_api::make_object<td_api::setTdlibParameters>(
        /*use_test_dc=*/false,
        /*database_directory=*/impl_->config.database_directory.string(),
        /*files_directory=*/std::string{},
        /*database_encryption_key=*/std::string{},
        /*use_file_database=*/true,
        /*use_chat_info_database=*/true,
        /*use_message_database=*/true,
        /*use_secret_chats=*/false,
        /*api_id=*/impl_->config.api_id,
        /*api_hash=*/impl_->config.api_hash,
        /*system_language_code=*/std::string{"en"},
        /*device_model=*/std::string{"CMLB"},
        /*system_version=*/std::string{},
        /*application_version=*/std::string{"1.0"});
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "set_tdlib_parameters");
}

asio::awaitable<core::Result<void>> TelegramGateway::check_bot_token() {
    auto fn = td_api::make_object<td_api::checkAuthenticationBotToken>(impl_->config.bot_token);
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_void(std::move(obj), "check_bot_token");
}

asio::awaitable<core::Result<void>> TelegramGateway::apply_runtime_options() {
    auto send_bool = [&](std::string name, bool value) -> asio::awaitable<core::Result<void>> {
        auto fn = td_api::make_object<td_api::setOption>(
            std::move(name), td_api::make_object<td_api::optionValueBoolean>(value));
        auto obj = co_await await_send(*impl_, std::move(fn));
        co_return response_to_void(std::move(obj), "setOption(bool)");
    };

    auto send_int = [&](std::string name,
                        std::int64_t value) -> asio::awaitable<core::Result<void>> {
        auto fn = td_api::make_object<td_api::setOption>(
            std::move(name), td_api::make_object<td_api::optionValueInteger>(value));
        auto obj = co_await await_send(*impl_, std::move(fn));
        co_return response_to_void(std::move(obj), "setOption(int)");
    };

    // ---- Throughput options ------------------------------------------------
    // Each failure is logged but never propagated — a single rejected option
    // (e.g. TDLib's name has changed across versions) must not stop the bot
    // from running.
    const std::pair<std::string_view, bool> kBoolOptions[] = {
        {"prefer_ipv6", impl_->config.prefer_ipv6},
        {"ignore_inline_thumbnails", true},
        {"ignore_background_updates", true},
        {"use_storage_optimizer", false},
        {"online", true},
        {"disable_persistent_network_statistics", false},
        {"disable_time_adjustment_protection", true},
    };
    for (const auto& [name, value] : kBoolOptions) {
        auto r = co_await send_bool(std::string{name}, value);
        if (!r.has_value()) {
            core::Logger::warn("TDLib setOption({}) failed: {}", name, r.error().message);
        }
    }

    // Integer-valued throughput options. TDLib clamps out-of-range values
    // and ignores unknown names across minor versions; the warn log captures
    // either case so operators can see what was rejected.
    const std::pair<std::string_view, std::int64_t> kIntOptions[] = {
        {"upload_chunk_size_kb", impl_->config.upload_chunk_size_kb},
        {"download_chunk_size_kb", impl_->config.download_chunk_size_kb},
        {"connection_retry_count_max", impl_->config.connection_retry_count_max},
    };
    for (const auto& [name, value] : kIntOptions) {
        auto r = co_await send_int(std::string{name}, value);
        if (!r.has_value()) {
            core::Logger::warn("TDLib setOption({}={}) failed: {}", name, value, r.error().message);
        }
    }

    // ---- Network type ------------------------------------------------------
    // `networkTypeWiFi` is the highest-bandwidth profile TDLib applies
    // internally (it tunes connection counts and chunk sizes). Bots running
    // on cellular links can override at runtime by surfacing this method to
    // a future `/network` admin command.
    auto net_fn =
        td_api::make_object<td_api::setNetworkType>(td_api::make_object<td_api::networkTypeWiFi>());
    auto net_obj = co_await await_send(*impl_, std::move(net_fn));
    if (auto r = response_to_void(std::move(net_obj), "setNetworkType"); !r.has_value()) {
        core::Logger::warn("TDLib setNetworkType failed: {}", r.error().message);
    }

    core::Logger::info("TDLib runtime options applied (prefer_ipv6={}, "
                       "upload_chunk_size_kb={}, download_chunk_size_kb={}, "
                       "connection_retry_count_max={}, networkTypeWiFi).",
                       impl_->config.prefer_ipv6,
                       impl_->config.upload_chunk_size_kb,
                       impl_->config.download_chunk_size_kb,
                       impl_->config.connection_retry_count_max);
    co_return core::Result<void>{};
}

asio::awaitable<core::Result<domain::MessageId>> TelegramGateway::send_file(
    domain::ChatId chat,
    std::filesystem::path file_path,
    std::string caption,
    std::optional<std::filesystem::path> thumbnail) {
    auto input_file = td_api::make_object<td_api::inputFileLocal>(file_path.string());

    td_api::object_ptr<td_api::inputThumbnail> thumb_obj;
    if (thumbnail.has_value()) {
        auto thumb_file = td_api::make_object<td_api::inputFileLocal>(thumbnail->string());
        thumb_obj = td_api::make_object<td_api::inputThumbnail>(std::move(thumb_file),
                                                                /*width=*/0,
                                                                /*height=*/0);
    }

    auto caption_ft = parse_html(caption);

    auto content =
        td_api::make_object<td_api::inputMessageDocument>(std::move(input_file),
                                                          std::move(thumb_obj),
                                                          /*disable_content_type_detection=*/false,
                                                          std::move(caption_ft));

    auto fn = td_api::make_object<td_api::sendMessage>(chat.value(),
                                                       /*topic_id=*/nullptr,
                                                       /*reply_to=*/nullptr,
                                                       /*options=*/nullptr,
                                                       /*reply_markup=*/nullptr,
                                                       std::move(content));
    auto obj = co_await await_send(*impl_, std::move(fn));
    co_return response_to_message_id(std::move(obj), "send_file");
}

} // namespace cmlb::infrastructure::telegram
