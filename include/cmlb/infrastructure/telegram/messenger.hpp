#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>

/// @file messenger.hpp
/// @brief Ergonomic façade on top of `TelegramGateway`.
///
/// `Messenger` exists so use cases never need to construct
/// `std::vector<std::vector<std::pair<...>>>` keyboards by hand and never see
/// the gateway's raw button representation. It adds zero behaviour beyond
/// keyboard translation and convenience defaults.

namespace cmlb::infrastructure::telegram {

/// A single inline keyboard button.
struct InlineKeyboardButton {
    std::string label;         ///< User-visible text on the button.
    std::string callback_data; ///< Payload sent back as `data` on press.
};

/// One horizontal row of inline keyboard buttons.
using InlineKeyboardRow = std::vector<InlineKeyboardButton>;

/// Stack of rows forming the full inline keyboard.
using InlineKeyboard = std::vector<InlineKeyboardRow>;

/// Polymorphic seam over the messenger API so that use cases can be unit-
/// tested with a stub implementation that records calls instead of touching
/// the real TDLib client.
///
/// `Messenger` (below) is the concrete production implementation that
/// forwards to `TelegramGateway`. Tests inject a `StubMessenger` that
/// implements the same surface in memory.
class MessengerInterface {
public:
    virtual ~MessengerInterface() = default;

    MessengerInterface() = default;
    MessengerInterface(const MessengerInterface&) = delete;
    MessengerInterface& operator=(const MessengerInterface&) = delete;
    MessengerInterface(MessengerInterface&&) = delete;
    MessengerInterface& operator=(MessengerInterface&&) = delete;

    /// Sends an HTML-formatted message. See `Messenger::send_html`.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<domain::MessageId>> send_html(
        domain::ChatId chat, std::string html) = 0;

    /// Edits an existing HTML message.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> edit_html(
        domain::ChatId chat, domain::MessageId msg, std::string html) = 0;

    /// Edits an existing HTML message and preserves/replaces its inline keyboard.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> edit_html_with_keyboard(
        domain::ChatId chat, domain::MessageId msg, std::string html, InlineKeyboard kb) = 0;

    /// Sends an HTML message with an attached inline keyboard.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<domain::MessageId>>
    send_html_with_keyboard(domain::ChatId chat, std::string html, InlineKeyboard kb) = 0;

    /// Replaces the inline keyboard attached to a message.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> edit_keyboard(
        domain::ChatId chat, domain::MessageId msg, InlineKeyboard kb) = 0;

    /// Acknowledges a callback query.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> answer_callback(
        domain::CallbackQueryId id, std::string text, bool alert) = 0;

    /// Uploads a file as a document.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<domain::MessageId>> send_file(
        domain::ChatId chat,
        std::filesystem::path file,
        std::string caption,
        std::optional<std::filesystem::path> thumbnail) = 0;

    /// Deletes a message.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> delete_message(
        domain::ChatId chat, domain::MessageId msg) = 0;
};

/// Convenience wrapper. Owns nothing — references the gateway externally.
///
/// Implements `MessengerInterface` so use cases can hold a `MessengerInterface&`
/// and remain unit-testable.
class Messenger final : public MessengerInterface {
public:
    /// Binds the messenger to a gateway. Lifetime: `gateway` must outlive
    /// this object (typically both live for the bot process lifetime).
    explicit Messenger(TelegramGateway& gateway) noexcept;

    ~Messenger() override = default;

    /// Sends an HTML-formatted message. Wraps
    /// `TelegramGateway::send_formatted_message`.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>> send_html(
        domain::ChatId chat, std::string html) override;

    /// Edits an existing HTML message.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> edit_html(domain::ChatId chat,
                                                                       domain::MessageId msg,
                                                                       std::string html) override;

    /// Edits an HTML message while setting its inline keyboard in the same request.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> edit_html_with_keyboard(
        domain::ChatId chat, domain::MessageId msg, std::string html, InlineKeyboard kb) override;

    /// Sends an HTML message with an attached inline keyboard.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>> send_html_with_keyboard(
        domain::ChatId chat, std::string html, InlineKeyboard kb) override;

    /// Replaces the inline keyboard attached to a message.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> edit_keyboard(
        domain::ChatId chat, domain::MessageId msg, InlineKeyboard kb) override;

    /// Acknowledges a callback query, optionally surfacing a toast/alert.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> answer_callback(
        domain::CallbackQueryId id, std::string text = {}, bool alert = false) override;

    /// Uploads a file as a document.
    [[nodiscard]] boost::asio::awaitable<core::Result<domain::MessageId>> send_file(
        domain::ChatId chat,
        std::filesystem::path file,
        std::string caption = {},
        std::optional<std::filesystem::path> thumbnail = std::nullopt) override;

    /// Deletes a message.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> delete_message(
        domain::ChatId chat, domain::MessageId msg) override;

    /// Builds a [Refresh][Close] button row. Used for status messages whose
    /// refresh action is keyed by a freshly minted token; the close action
    /// is fixed at the literal callback data `"close"`.
    [[nodiscard]] static InlineKeyboard refresh_close_row(std::string refresh_data);

private:
    /// Translates the ergonomic keyboard type to the gateway's raw form.
    [[nodiscard]] static std::vector<std::vector<std::pair<std::string, std::string>>> to_raw(
        const InlineKeyboard& kb);

    TelegramGateway& gateway_;
};

} // namespace cmlb::infrastructure::telegram
