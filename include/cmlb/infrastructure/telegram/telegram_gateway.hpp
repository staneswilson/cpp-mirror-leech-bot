#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>

/// @file telegram_gateway.hpp
/// @brief Thin, typed, PIMPL-isolated wrapper around `td::ClientManager`.
///
/// `TelegramGateway` is the **only** class in the project that includes the
/// TDLib headers (`<td/telegram/td_api.h>` and `<td/telegram/Client.h>`). Every
/// other module talks to Telegram through this gateway or one of the
/// higher-level helpers built on top of it (`Messenger`, `UpdateRouter`,
/// `AuthenticationFlow`).
///
/// Concurrency:
///  * TDLib's `ClientManager` is *not* thread-safe per `client_id`. All
///    `send` / `receive` calls are serialized on a dedicated strand
///    (`telegram_strand_` in the impl).
///  * Public methods are awaitables that internally `co_await` onto that
///    strand before touching the client.
///
/// Pending requests:
///  * Each outgoing request is recorded in a handler map keyed by an
///    application-issued `uint64_t` request id starting at 1000 (lower ids
///    are reserved for internal TDLib bookkeeping).
///  * A periodic eviction task completes pending handlers older than the
///    configurable TTL (default 30 s) with `ErrorCode::Timeout` so the
///    map's growth is bounded.

namespace cmlb::infrastructure::telegram {

/// Sole interface to TDLib. Owned by the bot composition root; lives for the
/// process lifetime.
class TelegramGateway {
public:
    /// Constructs the gateway. The TDLib client is *not* yet created —
    /// `run()` performs the bring-up handshake.
    TelegramGateway(core::Executor& executor, core::TelegramConfig config);

    /// Stops the receive loop, joins outstanding handlers, destroys the
    /// underlying client. Callers must ensure that the `run()` awaitable has
    /// returned (or that the parent `Executor` has been stopped) before the
    /// destructor runs — otherwise a strand-bound timer handler could touch
    /// freed PIMPL state.
    ~TelegramGateway();

    TelegramGateway(const TelegramGateway&)            = delete;
    TelegramGateway& operator=(const TelegramGateway&) = delete;
    TelegramGateway(TelegramGateway&&)                 = delete;
    TelegramGateway& operator=(TelegramGateway&&)      = delete;

    /// Runs the TDLib receive loop until `request_stop()` is invoked or
    /// cancellation is propagated. Call once at startup via `co_spawn`.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> run();

    /// Request graceful shutdown. Idempotent — safe to call from any thread.
    /// The awaitable returned by `run()` will complete shortly afterwards.
    void request_stop() noexcept;

    // ------------------------------------------------------------------
    // Typed request wrappers — never expose `td_api::Function` directly.
    // ------------------------------------------------------------------

    /// Sends a plain-text message. Returns the assigned message id.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>>
        send_text_message(domain::ChatId chat, std::string text);

    /// Sends an HTML-formatted message. Supports `<b>`, `<i>`, `<code>`,
    /// `<pre>`. Malformed HTML falls back to plain text — the send is *not*
    /// failed by parse errors.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>>
        send_formatted_message(domain::ChatId chat, std::string html);

    /// Replaces the text content of an existing message with new HTML.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
        edit_formatted_message(domain::ChatId chat,
                               domain::MessageId msg,
                               std::string html);

    /// Sends an HTML message with an attached inline keyboard. Each cell is
    /// `(label, callback_data)`.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>>
        send_message_with_inline_keyboard(
            domain::ChatId chat,
            std::string html,
            std::vector<std::vector<std::pair<std::string, std::string>>> rows);

    /// Replaces the inline keyboard attached to a message.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
        edit_message_inline_keyboard(
            domain::ChatId chat,
            domain::MessageId msg,
            std::vector<std::vector<std::pair<std::string, std::string>>> rows);

    /// Answers a callback query. `show_alert` controls whether Telegram
    /// renders the text in a modal.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
        answer_callback_query(domain::CallbackQueryId id,
                              std::string text,
                              bool show_alert);

    /// Deletes a message in the given chat.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
        delete_message(domain::ChatId chat, domain::MessageId msg);

    /// Uploads a local file as a document. Optional thumbnail attached.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>>
        send_file(domain::ChatId chat,
                  std::filesystem::path file_path,
                  std::string caption,
                  std::optional<std::filesystem::path> thumbnail);

    // ------------------------------------------------------------------
    // Update plumbing
    // ------------------------------------------------------------------

    /// User-facing handler signatures. Set once at startup with
    /// `set_update_handlers`. Handlers are invoked on the gateway strand and
    /// **must** be non-blocking — schedule heavy work onto the executor.
    struct UpdateHandlers {
        /// Fired for every incoming text message.
        std::function<void(domain::ChatId chat,
                           domain::UserId sender,
                           domain::MessageId msg_id,
                           std::string text)>
            on_new_message;

        /// Fired for every inline-keyboard callback press. `msg_id` identifies
        /// the message that owns the inline keyboard (so handlers can delete
        /// or edit it). It's `MessageId{0}` if TDLib reported no associated
        /// message (rare; defensive default).
        std::function<void(domain::ChatId chat,
                           domain::UserId sender,
                           domain::MessageId msg_id,
                           domain::CallbackQueryId query_id,
                           std::string data)>
            on_callback_query;

        /// Fired on TDLib `updateFile` notifications (upload/download progress).
        std::function<void(domain::FileId file_id,
                           std::int64_t total,
                           std::int64_t downloaded,
                           std::int64_t uploaded,
                           bool is_uploading_active,
                           bool is_downloading_active,
                           bool is_uploading_completed,
                           bool is_downloading_completed)>
            on_file_update;
    };

    /// Installs (or replaces) the update handlers. Thread-safe; the swap is
    /// posted onto the gateway strand.
    void set_update_handlers(UpdateHandlers handlers);

    /// Tag identifying an authorisation state surfaced by TDLib. The flow is:
    /// `WaitTdlibParameters` -> `WaitPhoneOrBotToken` -> `Ready` (or
    /// `LoggingOut` / `Closing`).
    enum class AuthState {
        WaitTdlibParameters,
        WaitPhoneOrBotToken,
        Ready,
        LoggingOut,
        Closing,
        Closed,
        Other
    };

    /// Internal authorisation-state listener. Used exclusively by
    /// `AuthenticationFlow` to drive the login state machine. Setting this to
    /// a null function disconnects the listener.
    using AuthStateHandler = std::function<void(AuthState)>;

    /// Registers (or clears) the auth-state listener.
    void set_auth_state_handler(AuthStateHandler handler);

    /// Sends a `setTdlibParameters` request with the values from the
    /// configuration passed at construction time. Returns once TDLib has
    /// acknowledged. Used by `AuthenticationFlow`; not normally called
    /// directly.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> set_tdlib_parameters();

    /// Sends `checkAuthenticationBotToken` with the configured bot token.
    /// Returns once TDLib has acknowledged. Used by `AuthenticationFlow`.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> check_bot_token();

    /// Pushes throughput-oriented runtime options to TDLib. Idempotent;
    /// `AuthenticationFlow` invokes it once after `authorizationStateReady`.
    ///
    /// Tuned for "fastest practical transfers":
    ///  - `prefer_ipv6 = true`           — shorter routes where both sides
    ///                                     have v6, avoids CG-NAT bottlenecks.
    ///  - `ignore_inline_thumbnails`     — skip lazily-decoded thumbnail blobs
    ///                                     we never render.
    ///  - `ignore_background_updates`    — drop "user came online" noise that
    ///                                     would otherwise wake the event loop.
    ///  - `use_storage_optimizer = false`— keep cached file parts; rebuilding
    ///                                     them on every restart trashes
    ///                                     incremental upload progress.
    ///  - `online = true`                — Telegram throttles "offline"
    ///                                     accounts hard. Bots that go idle
    ///                                     drop into a slower DC route.
    ///  - `setNetworkType(networkTypeWiFi)` — highest-bandwidth profile.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> apply_runtime_options();

    /// Default time-to-live for pending request handlers. Exposed so callers
    /// can document or override the default via the constructor (currently
    /// hard-coded; reserved for future configuration plumbing).
    static constexpr std::chrono::seconds kDefaultRequestTtl{30};

    /// Pending-request eviction sweep cadence.
    static constexpr std::chrono::seconds kEvictionInterval{5};

    // Forward-declared PIMPL; defined in telegram_gateway.cpp. Public so the
    // .cpp's anonymous-namespace helpers can name the type by reference.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cmlb::infrastructure::telegram
