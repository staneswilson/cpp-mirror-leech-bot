#pragma once

#include <cstddef>

#include <boost/asio/awaitable.hpp>

#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

/// @file cancel_task.hpp
/// @brief CancelTask use case — terminate a Task and free the underlying
///        downloader resources.

namespace cmlb::application {

struct CancelTaskRequest {
    cmlb::domain::TaskId task_id;
    cmlb::domain::ChatId chat;
};

/// Result of `CancelTask::cancel_all` — how many tasks the bulk operation
/// successfully cancelled.
struct CancelAllResult {
    std::size_t cancelled{0};
    std::size_t failed{0};
};

class CancelTask {
public:
    CancelTask(cmlb::infrastructure::persistence::TaskRepository& tasks,
               cmlb::infrastructure::download::DownloaderInterface& aria2,
               cmlb::infrastructure::download::DownloaderInterface& qbit,
               cmlb::infrastructure::telegram::MessengerInterface& messenger,
               ActiveTaskRegistry& active_tasks) noexcept;

    /// Cancels a single task by id.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> execute(
        CancelTaskRequest request);

    /// Cancels every non-terminal task. Failures on individual tasks are
    /// logged and accumulated in `CancelAllResult::failed`; a global error
    /// (e.g. repository unreachable) is returned via the outer `Result`.
    /// A single summary message is sent to @p chat at the end.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<CancelAllResult>> cancel_all(
        cmlb::domain::ChatId chat);

private:
    cmlb::infrastructure::persistence::TaskRepository& tasks_;
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    ActiveTaskRegistry& active_tasks_;
};

} // namespace cmlb::application
