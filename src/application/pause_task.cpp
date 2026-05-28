// ---------------------------------------------------------------------------
// pause_task.cpp — PauseTask use case implementation.
// ---------------------------------------------------------------------------

#include <cmlb/application/pause_task.hpp>

#include <utility>

#include <fmt/format.h>

#include <cmlb/core/logger.hpp>
#include <cmlb/domain/task.hpp>

namespace cmlb::application {

namespace asio        = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace tg_ns       = cmlb::infrastructure::telegram;

PauseTask::PauseTask(cmlb::infrastructure::persistence::TaskRepository& tasks,
                     download_ns::DownloaderInterface& aria2,
                     download_ns::DownloaderInterface& qbit,
                     tg_ns::MessengerInterface& messenger) noexcept
    : tasks_{tasks},
      aria2_{aria2},
      qbit_{qbit},
      messenger_{messenger} {}

asio::awaitable<cmlb::core::Result<void>>
PauseTask::execute(PauseTaskRequest request) {
    cmlb::core::Logger::info("pause_task: task={} chat={}",
                             request.task_id.value(),
                             request.chat.value());

    auto loaded = co_await tasks_.find(request.task_id);
    if (!loaded) co_return std::unexpected(loaded.error());
    if (!loaded->has_value()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                    "pause: task not found");
    }
    const auto& task = loaded->value();
    if (task.metadata().chat != request.chat) {
        cmlb::core::Logger::warn(
            "pause_task: cross-chat denied task={} task_chat={} request_chat={}",
            request.task_id.value(),
            task.metadata().chat.value(),
            request.chat.value());
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::PermissionDenied,
            "pause: task belongs to a different chat");
    }
    if (task.is_terminal()) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::InvalidState,
            "pause: task already in a terminal state");
    }
    const auto kind  = task.downloader_kind();
    const auto did   = task.downloader_id();
    if (!did.has_value() || kind == cmlb::domain::DownloaderKind::None) {
        cmlb::core::Logger::warn(
            "pause_task: task={} has no downloader binding yet",
            request.task_id.value());
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::InvalidState,
            "pause: task has no downloader binding (still queued?)");
    }

    cmlb::core::Result<void> backend{};
    switch (kind) {
        case cmlb::domain::DownloaderKind::Aria2:
            backend = co_await aria2_.pause(*did);
            break;
        case cmlb::domain::DownloaderKind::Qbittorrent:
            backend = co_await qbit_.pause(*did);
            break;
        case cmlb::domain::DownloaderKind::None:
            // Unreachable (guarded above).
            break;
    }

    if (!backend) {
        cmlb::core::Logger::warn("pause_task: backend refused: {}",
                                 backend.error().message);
        co_return std::unexpected(backend.error());
    }

    // The Task state machine has no `Paused` state — the task continues to
    // live in Downloading per the domain spec.

    (void)co_await messenger_.send_html(
        request.chat,
        fmt::format("<b>Paused</b>: <code>{}</code>",
                    request.task_id.value()));
    cmlb::core::Logger::info("pause_task: task={} paused (backend hint sent)",
                             request.task_id.value());
    co_return cmlb::core::Result<void>{};
}

}  // namespace cmlb::application
