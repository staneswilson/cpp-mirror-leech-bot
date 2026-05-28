// ---------------------------------------------------------------------------
// callback_dispatcher.cpp
//
// Routes inline-keyboard callback payloads to use cases. Every dispatch path
// ends in `messenger.answer_callback(query_id, "", false)` so Telegram's
// loading spinner is dismissed even when no action is taken.
// ---------------------------------------------------------------------------

#include <cmlb/presentation/callback_dispatcher.hpp>

#include <string>
#include <string_view>
#include <utility>

#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/upload_destination.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

#include <cmlb/application/cancel_task.hpp>
#include <cmlb/application/pause_task.hpp>
#include <cmlb/application/resume_task.hpp>
#include <cmlb/application/update_user_settings.hpp>

namespace cmlb::presentation {

namespace asio = boost::asio;
using cmlb::core::AppError;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;
using cmlb::domain::UploadDestination;

namespace {

/// Splits @p data on the first `:` character.
struct PayloadSplit {
    std::string_view head;
    std::string_view tail;
};

[[nodiscard]] PayloadSplit split_colon(std::string_view data) noexcept {
    const auto sep = data.find(':');
    if (sep == std::string_view::npos) {
        return {data, {}};
    }
    return {data.substr(0, sep), data.substr(sep + 1)};
}

/// Cycles `Telegram -> GoogleDrive -> Rclone -> Telegram`.
[[nodiscard]] UploadDestination next_destination(UploadDestination current) noexcept {
    switch (current) {
        case UploadDestination::Telegram:    return UploadDestination::GoogleDrive;
        case UploadDestination::GoogleDrive: return UploadDestination::Rclone;
        case UploadDestination::Rclone:      return UploadDestination::Telegram;
    }
    return UploadDestination::Telegram;
}

}  // namespace

CallbackDispatcher::CallbackDispatcher(Dependencies deps) noexcept
    : deps_{std::move(deps)} {}

CallbackDispatcher::~CallbackDispatcher() = default;

asio::awaitable<Result<void>>
CallbackDispatcher::dispatch(cmlb::domain::ChatId          chat,
                             cmlb::domain::UserId          sender,
                             cmlb::domain::MessageId       msg_id,
                             cmlb::domain::CallbackQueryId query_id,
                             std::string                   data) {
    using namespace cmlb::application;

    Logger::debug("callback: chat={} user={} msg={} data=\"{}\"",
                  chat.value(), sender.value(), msg_id.value(), data);

    Result<void> outcome{};
    std::string  ack_text;          // toast surfaced to the user
    bool         ack_as_alert{false};
    const auto [head, rest] = split_colon(data);

    if (head == "close") {
        // Delete the host message. If TDLib reported no associated message
        // (msg_id == 0) the call is a no-op — the spinner-ack at the bottom
        // still fires.
        if (msg_id.value() != 0) {
            auto del = co_await deps_.messenger.delete_message(chat, msg_id);
            if (!del) {
                Logger::warn("callback: close: delete_message failed: {}",
                             del.error().message);
                outcome = std::unexpected(del.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(del.error().code)};
                ack_as_alert = true;
            }
        }
    } else if (head == "status" && rest == "refresh") {
        // `ProgressRenderer` repaints autonomously; surface a brief toast so
        // the user knows the tap registered.
        ack_text = "Refreshing…";
    } else if (head == "settings") {
        const auto [section, action] = split_colon(rest);
        if (section == "upload" && action == "cycle") {
            UpdateUserSettingsRequest req{
                .user = sender,
                .mutate = [](auto& rec) noexcept {
                    rec.mirror_destination =
                        next_destination(rec.mirror_destination);
                },
            };
            auto res = co_await deps_.update_user.execute(std::move(req));
            if (!res) {
                outcome = std::unexpected(res.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(res.error().code)};
                ack_as_alert = true;
            } else {
                ack_text = "Upload destination updated";
            }
        } else if (section == "doc" && action == "toggle") {
            UpdateUserSettingsRequest req{
                .user = sender,
                .mutate = [](auto& rec) noexcept {
                    rec.upload_as_document = !rec.upload_as_document;
                },
            };
            auto res = co_await deps_.update_user.execute(std::move(req));
            if (!res) {
                outcome = std::unexpected(res.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(res.error().code)};
                ack_as_alert = true;
            } else {
                ack_text = "Setting toggled";
            }
        } else {
            Logger::debug("callback: unknown settings action \"{}\"", data);
            ack_text = "Unknown action";
        }
    } else if (head == "task") {
        const auto [verb, task_id_text] = split_colon(rest);
        if (task_id_text.empty()) {
            Logger::debug("callback: task action missing id (\"{}\")", data);
            ack_text = "Invalid task action";
            ack_as_alert = true;
        } else if (verb == "cancel") {
            CancelTaskRequest req{
                .task_id = cmlb::domain::TaskId{std::string{task_id_text}},
                .chat    = chat,
            };
            auto res = co_await deps_.cancel_task.execute(std::move(req));
            if (!res) {
                outcome = std::unexpected(res.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(res.error().code)};
                ack_as_alert = true;
            } else {
                ack_text = "Cancelling task";
            }
        } else if (verb == "pause") {
            PauseTaskRequest req{
                .task_id = cmlb::domain::TaskId{std::string{task_id_text}},
                .chat    = chat,
            };
            auto res = co_await deps_.pause_task.execute(std::move(req));
            if (!res) {
                outcome = std::unexpected(res.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(res.error().code)};
                ack_as_alert = true;
            } else {
                ack_text = "Task paused";
            }
        } else if (verb == "resume") {
            ResumeTaskRequest req{
                .task_id = cmlb::domain::TaskId{std::string{task_id_text}},
                .chat    = chat,
            };
            auto res = co_await deps_.resume_task.execute(std::move(req));
            if (!res) {
                outcome = std::unexpected(res.error());
                ack_text = std::string{
                    cmlb::core::friendly_error_label(res.error().code)};
                ack_as_alert = true;
            } else {
                ack_text = "Task resumed";
            }
        } else {
            Logger::debug("callback: unknown task verb \"{}\"", verb);
            ack_text = "Unknown action";
        }
    } else {
        Logger::debug("callback: unknown payload \"{}\"", data);
        ack_text = "Unknown action";
    }

    // Acknowledge the callback regardless of outcome — the toast carries the
    // outcome message; alert=true is reserved for actual failures.
    auto ack = co_await deps_.messenger.answer_callback(
        query_id, std::move(ack_text), ack_as_alert);
    if (!ack && outcome.has_value()) {
        outcome = std::unexpected(ack.error());
    }
    co_return outcome;
}

}  // namespace cmlb::presentation
