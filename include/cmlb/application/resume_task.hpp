#pragma once

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

/// @file resume_task.hpp
/// @brief ResumeTask use case — resumes a previously-paused downloading task.

namespace cmlb::application {

struct ResumeTaskRequest {
    cmlb::domain::TaskId task_id;
    cmlb::domain::ChatId chat;
};

class ResumeTask {
public:
    ResumeTask(cmlb::infrastructure::persistence::TaskRepository& tasks,
               cmlb::infrastructure::download::DownloaderInterface& aria2,
               cmlb::infrastructure::download::DownloaderInterface& qbit,
               cmlb::infrastructure::telegram::MessengerInterface& messenger) noexcept;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> execute(
        ResumeTaskRequest request);

private:
    cmlb::infrastructure::persistence::TaskRepository& tasks_;
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
};

} // namespace cmlb::application
