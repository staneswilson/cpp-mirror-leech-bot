#pragma once

#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>

/// @file callback_dispatcher.hpp
/// @brief Inline-keyboard callback router.
///
/// The Telegram update layer hands every `callback_query` event to
/// `CallbackDispatcher::dispatch`. The dispatcher parses the colon-separated
/// `data` payload, routes to the matching handler, and *always* answers the
/// callback to make Telegram's spinner stop.

namespace cmlb::application {
class CancelTask;
class PauseTask;
class ResumeTask;
class UpdateUserSettings;
}  // namespace cmlb::application

namespace cmlb::infrastructure::telegram {
class MessengerInterface;
}  // namespace cmlb::infrastructure::telegram

namespace cmlb::presentation {

/// Routes Telegram inline-button callbacks to use cases.
///
/// Recognised payload formats (`:`-delimited):
///  - `close`                       — deletes the host message (the one whose
///                                    inline keyboard the user pressed).
///  - `settings:upload:cycle`       — cycles the user's mirror destination.
///  - `settings:doc:toggle`         — flips `upload_as_document`.
///  - `task:cancel:<task_id>`       — cancels the named task.
///  - `task:pause:<task_id>`        — pauses the named task.
///  - `task:resume:<task_id>`       — resumes the named task.
///  - `status:refresh`              — no-op; refresh is driven by the
///                                    `ProgressRenderer` background updater.
///
/// Unknown payloads are silently ignored (apart from the obligatory callback
/// acknowledgement).
class CallbackDispatcher {
public:
    /// External collaborators. References must outlive the dispatcher.
    struct Dependencies {
        cmlb::application::CancelTask&                      cancel_task;
        cmlb::application::PauseTask&                       pause_task;
        cmlb::application::ResumeTask&                      resume_task;
        cmlb::application::UpdateUserSettings&              update_user;
        cmlb::infrastructure::telegram::MessengerInterface& messenger;
    };

    explicit CallbackDispatcher(Dependencies deps) noexcept;
    ~CallbackDispatcher();

    CallbackDispatcher(const CallbackDispatcher&)            = delete;
    CallbackDispatcher& operator=(const CallbackDispatcher&) = delete;
    CallbackDispatcher(CallbackDispatcher&&)                 = delete;
    CallbackDispatcher& operator=(CallbackDispatcher&&)      = delete;

    /// Dispatches @p data to the appropriate handler.
    ///
    /// Always answers the callback query (via the messenger) before returning,
    /// even when the payload is unknown or a handler fails. A failing
    /// acknowledgement is returned only when no other error already exists.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>>
        dispatch(cmlb::domain::ChatId          chat,
                 cmlb::domain::UserId          sender,
                 cmlb::domain::MessageId       msg_id,
                 cmlb::domain::CallbackQueryId query_id,
                 std::string                   data);

private:
    Dependencies deps_;
};

}  // namespace cmlb::presentation
