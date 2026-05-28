// SPDX-License-Identifier: MIT
//
// Build-time stub for `TelegramGateway` — keeps the link graph green on
// hosts without TDLib. Every method returns `ErrorCode::Internal`. Selected
// at configure time when TDLib is unavailable; reconfigure with
// `-DCMLB_WITH_TELEGRAM=ON` (or `CMLB_PREFER_REAL_TDLIB=1` env) to use the
// real implementation in `telegram_gateway.cpp`.

#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/logger.hpp>

namespace cmlb::infrastructure::telegram {

namespace {

constexpr const char* kStubMessage =
    "telegram gateway stub: rebuild with -DCMLB_WITH_TELEGRAM=ON";

template <typename T>
[[nodiscard]] core::Result<T> stub_error() {
    return core::error(core::ErrorCode::Internal, kStubMessage);
}

}  // namespace

struct TelegramGateway::Impl {
    core::Executor*       executor;
    core::TelegramConfig  config;
    UpdateHandlers        updates;
    AuthStateHandler      auth_handler;
    std::mutex            mutex;
    bool                  stop_requested{false};

    Impl(core::Executor& exec, core::TelegramConfig cfg)
        : executor{&exec}, config{std::move(cfg)} {}
};

TelegramGateway::TelegramGateway(core::Executor& executor,
                                 core::TelegramConfig config)
    : impl_{std::make_unique<Impl>(executor, std::move(config))} {
    core::Logger::warn("TelegramGateway: stub implementation active.");
}

TelegramGateway::~TelegramGateway() = default;

boost::asio::awaitable<core::Result<void>> TelegramGateway::run() {
    // Park until request_stop; matches the real gateway's blocking semantics
    // so the composition root behaves identically.
    auto exec = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer parker{exec};
    parker.expires_at(std::chrono::steady_clock::time_point::max());

    {
        std::lock_guard lk{impl_->mutex};
        if (impl_->stop_requested) {
            co_return core::Result<void>{};
        }
    }

    // Signal Closed so AuthenticationFlow doesn't wait forever for Ready.
    if (impl_->auth_handler) {
        impl_->auth_handler(AuthState::Closed);
    }

    boost::system::error_code ec;
    co_await parker.async_wait(boost::asio::redirect_error(
        boost::asio::use_awaitable, ec));
    co_return core::Result<void>{};
}

void TelegramGateway::request_stop() noexcept {
    std::lock_guard lk{impl_->mutex};
    impl_->stop_requested = true;
}

boost::asio::awaitable<core::Result<domain::MessageId>>
TelegramGateway::send_text_message(domain::ChatId, std::string) {
    co_return stub_error<domain::MessageId>();
}

boost::asio::awaitable<core::Result<domain::MessageId>>
TelegramGateway::send_formatted_message(domain::ChatId, std::string) {
    co_return stub_error<domain::MessageId>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::edit_formatted_message(domain::ChatId,
                                        domain::MessageId,
                                        std::string) {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<domain::MessageId>>
TelegramGateway::send_message_with_inline_keyboard(
    domain::ChatId,
    std::string,
    std::vector<std::vector<std::pair<std::string, std::string>>>) {
    co_return stub_error<domain::MessageId>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::edit_message_inline_keyboard(
    domain::ChatId,
    domain::MessageId,
    std::vector<std::vector<std::pair<std::string, std::string>>>) {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::answer_callback_query(domain::CallbackQueryId,
                                       std::string,
                                       bool) {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::delete_message(domain::ChatId, domain::MessageId) {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<domain::MessageId>>
TelegramGateway::send_file(domain::ChatId,
                           std::filesystem::path,
                           std::string,
                           std::optional<std::filesystem::path>) {
    co_return stub_error<domain::MessageId>();
}

void TelegramGateway::set_update_handlers(UpdateHandlers handlers) {
    std::lock_guard lk{impl_->mutex};
    impl_->updates = std::move(handlers);
}

void TelegramGateway::set_auth_state_handler(AuthStateHandler handler) {
    std::lock_guard lk{impl_->mutex};
    impl_->auth_handler = std::move(handler);
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::set_tdlib_parameters() {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::check_bot_token() {
    co_return stub_error<void>();
}

boost::asio::awaitable<core::Result<void>>
TelegramGateway::apply_runtime_options() {
    // No-op: returning success avoids a misleading warning on startup.
    co_return core::Result<void>{};
}

}  // namespace cmlb::infrastructure::telegram
