#pragma once

#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/progress_renderer_interface.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/upload_destination.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

/// @file mirror_url.hpp
/// @brief MirrorUrl use case: download a URL/magnet then upload to a cloud
///        destination (Google Drive or rclone). The Telegram leech variant is
///        in `leech_url.hpp`.

namespace cmlb::application {

/// Request DTO for `MirrorUrl::execute`.
struct MirrorRequest {
    /// URL, magnet link, or aria2 input handed to the downloader verbatim.
    std::string url;
    /// User issuing the command (used to select user-scoped settings).
    cmlb::domain::UserId user;
    /// Chat in which the bot replies with progress messages.
    cmlb::domain::ChatId chat;
    /// The originating user message id — replied to or quoted by status output.
    cmlb::domain::MessageId source_message;
    /// `true` routes through the qBittorrent backend (the `/qbmirror` flow);
    /// `false` uses aria2 (default `/mirror`).
    bool use_qbittorrent{false};
    /// Per-invocation override; when unset the user's saved preference is
    /// consulted (which itself defaults to `GoogleDrive`).
    std::optional<cmlb::domain::UploadDestination> override_destination;
};

/// MirrorUrl orchestrates a download (aria2 or qBittorrent) followed by an
/// upload (GoogleDrive or rclone). All side-effecting collaborators are
/// constructor-injected; the class itself is pure orchestration.
class MirrorUrl {
public:
    MirrorUrl(cmlb::infrastructure::download::DownloaderInterface& aria2_downloader,
              cmlb::infrastructure::download::DownloaderInterface& qbit_downloader,
              cmlb::infrastructure::upload::UploaderInterface& gdrive_uploader,
              cmlb::infrastructure::upload::UploaderInterface& rclone_uploader,
              cmlb::infrastructure::persistence::TaskRepository& tasks,
              cmlb::infrastructure::persistence::UserSettingsRepository& user_settings,
              cmlb::infrastructure::telegram::MessengerInterface& messenger,
              cmlb::application::ProgressRendererInterface& progress_renderer,
              cmlb::core::Executor& executor,
              cmlb::application::ActiveTaskRegistry& active_tasks,
              int upload_pool_size = 4) noexcept;

    /// Runs the full pipeline and returns the assigned `TaskId`. On any
    /// non-recoverable failure the task is persisted in `Failed`/`Cancelled`
    /// state and the user is notified through `messenger`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> execute(
        MirrorRequest request);

private:
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::upload::UploaderInterface& gdrive_;
    cmlb::infrastructure::upload::UploaderInterface& rclone_;
    cmlb::infrastructure::persistence::TaskRepository& tasks_;
    cmlb::infrastructure::persistence::UserSettingsRepository& user_settings_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    cmlb::application::ProgressRendererInterface& progress_renderer_;
    cmlb::core::Executor& executor_;
    cmlb::application::ActiveTaskRegistry& active_tasks_;
    int upload_pool_size_;
};

} // namespace cmlb::application
