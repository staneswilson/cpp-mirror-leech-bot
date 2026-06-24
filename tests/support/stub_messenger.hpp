#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

namespace cmlb::test_support {

/// In-memory MessengerInterface used by application-layer unit tests.
/// Records every call and assigns monotonically increasing fake message ids.
class StubMessenger final : public cmlb::infrastructure::telegram::MessengerInterface {
public:
    struct HtmlSend {
        cmlb::domain::ChatId chat;
        std::string html;
        cmlb::domain::MessageId assigned;
    };

    struct HtmlEdit {
        cmlb::domain::ChatId chat;
        cmlb::domain::MessageId msg;
        std::string html;
    };

    struct HtmlKeyboardEdit {
        cmlb::domain::ChatId chat;
        cmlb::domain::MessageId msg;
        std::string html;
        cmlb::infrastructure::telegram::InlineKeyboard keyboard;
    };

    StubMessenger() = default;
    ~StubMessenger() override = default;

    // ---- inspection ----------------------------------------------------
    [[nodiscard]] std::vector<HtmlSend> sends() const {
        std::lock_guard lk{mutex_};
        return sends_;
    }

    [[nodiscard]] std::vector<HtmlEdit> edits() const {
        std::lock_guard lk{mutex_};
        return edits_;
    }

    [[nodiscard]] std::vector<HtmlKeyboardEdit> keyboard_edits() const {
        std::lock_guard lk{mutex_};
        return keyboard_edits_;
    }

    [[nodiscard]] std::vector<cmlb::domain::MessageId> deleted() const {
        std::lock_guard lk{mutex_};
        return deleted_;
    }

    // ---- MessengerInterface -------------------------------------------
    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::MessageId>> send_html(
        cmlb::domain::ChatId chat, std::string html) override {
        std::lock_guard lk{mutex_};
        cmlb::domain::MessageId mid{++next_id_};
        sends_.push_back(HtmlSend{chat, std::move(html), mid});
        co_return mid;
    }

    boost::asio::awaitable<cmlb::core::Result<void>> edit_html(cmlb::domain::ChatId chat,
                                                               cmlb::domain::MessageId msg,
                                                               std::string html) override {
        std::lock_guard lk{mutex_};
        edits_.push_back(HtmlEdit{chat, msg, std::move(html)});
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>> edit_html_with_keyboard(
        cmlb::domain::ChatId chat,
        cmlb::domain::MessageId msg,
        std::string html,
        cmlb::infrastructure::telegram::InlineKeyboard kb) override {
        std::lock_guard lk{mutex_};
        keyboard_edits_.push_back(
            HtmlKeyboardEdit{chat, msg, std::move(html), std::move(kb)});
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::MessageId>> send_html_with_keyboard(
        cmlb::domain::ChatId chat,
        std::string html,
        cmlb::infrastructure::telegram::InlineKeyboard) override {
        std::lock_guard lk{mutex_};
        cmlb::domain::MessageId mid{++next_id_};
        sends_.push_back(HtmlSend{chat, std::move(html), mid});
        co_return mid;
    }

    boost::asio::awaitable<cmlb::core::Result<void>> edit_keyboard(
        cmlb::domain::ChatId,
        cmlb::domain::MessageId,
        cmlb::infrastructure::telegram::InlineKeyboard) override {
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>> answer_callback(cmlb::domain::CallbackQueryId,
                                                                     std::string,
                                                                     bool) override {
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::MessageId>> send_file(
        cmlb::domain::ChatId chat,
        std::filesystem::path,
        std::string,
        std::optional<std::filesystem::path>) override {
        std::lock_guard lk{mutex_};
        cmlb::domain::MessageId mid{++next_id_};
        sends_.push_back(HtmlSend{chat, "<file>", mid});
        co_return mid;
    }

    boost::asio::awaitable<cmlb::core::Result<void>> delete_message(
        cmlb::domain::ChatId, cmlb::domain::MessageId msg) override {
        std::lock_guard lk{mutex_};
        deleted_.push_back(msg);
        co_return cmlb::core::Result<void>{};
    }

private:
    mutable std::mutex mutex_;
    std::int64_t next_id_{1000};

    std::vector<HtmlSend> sends_;
    std::vector<HtmlEdit> edits_;
    std::vector<HtmlKeyboardEdit> keyboard_edits_;
    std::vector<cmlb::domain::MessageId> deleted_;
};

} // namespace cmlb::test_support
