#pragma once

#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/progress_renderer_interface.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

/// @file leech_url.hpp
/// @brief LeechUrl use case: download a URL/magnet then re-upload it back to
///        Telegram. Mirrors `MirrorUrl` but the uploader is fixed.

namespace cmlb::application {

/// Request DTO for `LeechUrl::execute`.
struct LeechRequest {
    std::string url;
    cmlb::domain::UserId user;
    cmlb::domain::ChatId chat;
    cmlb::domain::MessageId source_message;
    /// `true` selects the qBittorrent backend (the `/qbleech` flow).
    bool use_qbittorrent{false};
};

/// LeechUrl downloads via aria2 or qBittorrent then forwards every file back
/// to the requesting chat through the Telegram uploader.
class LeechUrl {
public:
    LeechUrl(cmlb::infrastructure::download::DownloaderInterface& aria2_downloader,
             cmlb::infrastructure::download::DownloaderInterface& qbit_downloader,
             cmlb::infrastructure::upload::UploaderInterface& telegram_uploader,
             cmlb::infrastructure::persistence::TaskRepository& tasks,
             cmlb::infrastructure::persistence::UserSettingsRepository& user_settings,
             cmlb::infrastructure::telegram::MessengerInterface& messenger,
             cmlb::application::ProgressRendererInterface& progress_renderer,
             cmlb::core::Executor& executor,
             cmlb::application::ActiveTaskRegistry& active_tasks,
             int upload_pool_size = 4) noexcept;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> execute(
        LeechRequest request);

private:
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::upload::UploaderInterface& telegram_uploader_;
    cmlb::infrastructure::persistence::TaskRepository& tasks_;
    cmlb::infrastructure::persistence::UserSettingsRepository& user_settings_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    cmlb::application::ProgressRendererInterface& progress_renderer_;
    cmlb::core::Executor& executor_;
    cmlb::application::ActiveTaskRegistry& active_tasks_;
    int upload_pool_size_;
};

} // namespace cmlb::application
