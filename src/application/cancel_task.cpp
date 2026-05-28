// ---------------------------------------------------------------------------
// cancel_task.cpp — CancelTask use case implementation.
// ---------------------------------------------------------------------------

#include <utility>

#include <fmt/format.h>

#include <cmlb/application/cancel_task.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/task.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace tg_ns = cmlb::infrastructure::telegram;

CancelTask::CancelTask(cmlb::infrastructure::persistence::TaskRepository& tasks,
                       download_ns::DownloaderInterface& aria2,
                       download_ns::DownloaderInterface& qbit,
                       tg_ns::MessengerInterface& messenger,
                       ActiveTaskRegistry& active_tasks) noexcept
    : tasks_{tasks},
      aria2_{aria2},
      qbit_{qbit},
      messenger_{messenger},
      active_tasks_{active_tasks} {
}

asio::awaitable<cmlb::core::Result<void>> CancelTask::execute(CancelTaskRequest request) {
    cmlb::core::Logger::info(
        "cancel_task: task={} chat={}", request.task_id.value(), request.chat.value());

    auto loaded = co_await tasks_.find(request.task_id);
    if (!loaded)
        co_return std::unexpected(loaded.error());
    if (!loaded->has_value()) {
        cmlb::core::Logger::warn("cancel_task: task not found id={}", request.task_id.value());
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound, "cancel: task not found");
    }

    cmlb::domain::Task task = std::move(**loaded);
    // Chat-scope check: a Task belongs to exactly one chat. Cross-chat /cancel
    // is denied so a user in chat A cannot tear down a task running in chat B.
    if (task.metadata().chat != request.chat) {
        cmlb::core::Logger::warn(
            "cancel_task: cross-chat denied task={} task_chat={} request_chat={}",
            request.task_id.value(),
            task.metadata().chat.value(),
            request.chat.value());
        co_return cmlb::core::error(cmlb::core::ErrorCode::PermissionDenied,
                                    "cancel: task belongs to a different chat");
    }
    if (task.is_terminal()) {
        cmlb::core::Logger::warn("cancel_task: task={} already terminal", request.task_id.value());
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "cancel: task already in a terminal state");
    }

    // Signal the running use-case coroutine. If a coroutine is active, it
    // owns the teardown: it cancels in-flight uploads, calls
    // `downloader.remove`, marks the task `Cancelled`, persists, and edits
    // the status message. We just acknowledge and return — duplicating that
    // work here races on the DB write and double-edits the status message.
    if (active_tasks_.cancel(request.task_id)) {
        cmlb::core::Logger::info("cancel_task: task={} signalled in-flight coroutine",
                                 request.task_id.value());
        (void)co_await messenger_.send_html(
            request.chat,
            fmt::format("<b>Cancelling</b>: <code>{}</code>", request.task_id.value()));
        co_return cmlb::core::Result<void>{};
    }

    // No live coroutine — the task is parked (Queued/Paused) or somehow
    // orphaned from its use case. Perform the teardown here.
    const auto kind = task.downloader_kind();
    const auto did = task.downloader_id();
    if (did.has_value()) {
        switch (kind) {
        case cmlb::domain::DownloaderKind::Aria2:
            (void)co_await aria2_.remove(*did, true);
            break;
        case cmlb::domain::DownloaderKind::Qbittorrent:
            (void)co_await qbit_.remove(*did, true);
            break;
        case cmlb::domain::DownloaderKind::None:
            // Defensive: downloader_id set but kind not — bug elsewhere.
            cmlb::core::Logger::warn("cancel_task: task={} has gid but DownloaderKind::None",
                                     request.task_id.value());
            break;
        }
    } else {
        cmlb::core::Logger::info("cancel_task: task={} has no downloader binding; "
                                 "cancelling without backend remove",
                                 request.task_id.value());
    }

    if (auto mark = task.mark_cancelled(); !mark) {
        cmlb::core::Logger::error("cancel_task: state transition failed: {}", mark.error().message);
        co_return std::unexpected(mark.error());
    }
    auto saved = co_await tasks_.save(task);
    if (!saved)
        co_return std::unexpected(saved.error());

    (void)co_await messenger_.send_html(
        request.chat, fmt::format("<b>Cancelled</b>: <code>{}</code>", request.task_id.value()));
    cmlb::core::Logger::info("cancel_task: task={} cancelled", request.task_id.value());
    co_return cmlb::core::Result<void>{};
}

asio::awaitable<cmlb::core::Result<CancelAllResult>> CancelTask::cancel_all(
    cmlb::domain::ChatId chat) {
    cmlb::core::Logger::info("cancel_all: chat={}", chat.value());

    auto incomplete = co_await tasks_.incomplete();
    if (!incomplete)
        co_return std::unexpected(incomplete.error());

    CancelAllResult result;
    for (auto& task : *incomplete) {
        // `incomplete()` is repo-wide. Cancelling tasks across every chat from
        // a single `/cancelall` is destructive and unintended — restrict the
        // sweep to this chat only.
        if (task.metadata().chat != chat) {
            continue;
        }
        CancelTaskRequest req{
            .task_id = task.metadata().id,
            .chat = chat,
        };
        auto outcome = co_await execute(std::move(req));
        if (outcome.has_value()) {
            ++result.cancelled;
        } else {
            ++result.failed;
            cmlb::core::Logger::warn("cancel_all: task={} failed: {}",
                                     task.metadata().id.value(),
                                     outcome.error().message);
        }
    }

    (void)co_await messenger_.send_html(chat,
                                        fmt::format("<b>Bulk cancel</b>: {} cancelled, {} failed",
                                                    result.cancelled,
                                                    result.failed));
    cmlb::core::Logger::info("cancel_all: chat={} cancelled={} failed={}",
                             chat.value(),
                             result.cancelled,
                             result.failed);
    co_return result;
}

} // namespace cmlb::application
