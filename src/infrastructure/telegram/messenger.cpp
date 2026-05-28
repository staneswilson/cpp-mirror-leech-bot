// ---------------------------------------------------------------------------
// messenger.cpp
//
// Thin ergonomic façade over `TelegramGateway`. No TDLib headers here — all
// translation is delegated to the gateway.
// ---------------------------------------------------------------------------

#include <utility>

#include <cmlb/infrastructure/telegram/messenger.hpp>

namespace cmlb::infrastructure::telegram {

namespace asio = boost::asio;

Messenger::Messenger(TelegramGateway& gateway) noexcept : gateway_{gateway} {
}

std::vector<std::vector<std::pair<std::string, std::string>>> Messenger::to_raw(
    const InlineKeyboard& kb) {
    std::vector<std::vector<std::pair<std::string, std::string>>> rows;
    rows.reserve(kb.size());
    for (const auto& row : kb) {
        std::vector<std::pair<std::string, std::string>> raw_row;
        raw_row.reserve(row.size());
        for (const auto& button : row) {
            raw_row.emplace_back(button.label, button.callback_data);
        }
        rows.push_back(std::move(raw_row));
    }
    return rows;
}

asio::awaitable<core::Result<domain::MessageId>> Messenger::send_html(domain::ChatId chat,
                                                                      std::string html) {
    co_return co_await gateway_.send_formatted_message(chat, std::move(html));
}

asio::awaitable<core::Result<void>> Messenger::edit_html(domain::ChatId chat,
                                                         domain::MessageId msg,
                                                         std::string html) {
    co_return co_await gateway_.edit_formatted_message(chat, msg, std::move(html));
}

asio::awaitable<core::Result<domain::MessageId>> Messenger::send_html_with_keyboard(
    domain::ChatId chat, std::string html, InlineKeyboard kb) {
    co_return co_await gateway_.send_message_with_inline_keyboard(
        chat, std::move(html), to_raw(kb));
}

asio::awaitable<core::Result<void>> Messenger::edit_keyboard(domain::ChatId chat,
                                                             domain::MessageId msg,
                                                             InlineKeyboard kb) {
    co_return co_await gateway_.edit_message_inline_keyboard(chat, msg, to_raw(kb));
}

asio::awaitable<core::Result<void>> Messenger::answer_callback(domain::CallbackQueryId id,
                                                               std::string text,
                                                               bool alert) {
    co_return co_await gateway_.answer_callback_query(id, std::move(text), alert);
}

asio::awaitable<core::Result<domain::MessageId>> Messenger::send_file(
    domain::ChatId chat,
    std::filesystem::path file,
    std::string caption,
    std::optional<std::filesystem::path> thumbnail) {
    co_return co_await gateway_.send_file(
        chat, std::move(file), std::move(caption), std::move(thumbnail));
}

asio::awaitable<core::Result<void>> Messenger::delete_message(domain::ChatId chat,
                                                              domain::MessageId msg) {
    co_return co_await gateway_.delete_message(chat, msg);
}

InlineKeyboard Messenger::refresh_close_row(std::string refresh_data) {
    return InlineKeyboard{
        InlineKeyboardRow{InlineKeyboardButton{"Refresh", std::move(refresh_data)},
                          InlineKeyboardButton{"Close", "close"}}};
}

} // namespace cmlb::infrastructure::telegram
