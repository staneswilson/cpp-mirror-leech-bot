#pragma once

#include <chrono>
#include <optional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

/// @file show_status.hpp
/// @brief ShowStatus use case — render fast, on-demand task status.

namespace cmlb::application {

/// Request DTO for `ShowStatus::execute`.
struct StatusRequest {
    cmlb::domain::UserId user;
    cmlb::domain::ChatId chat;
    std::optional<cmlb::domain::TaskId> task_id{std::nullopt};
    bool include_all_users{false};
};

/// Reads persisted active tasks, refreshes downloader snapshots, and sends
/// the `/status` response to Telegram.
class ShowStatus {
public:
    ShowStatus(cmlb::infrastructure::persistence::TaskRepository& tasks,
               cmlb::infrastructure::download::DownloaderInterface& aria2,
               cmlb::infrastructure::download::DownloaderInterface& qbit,
               cmlb::infrastructure::telegram::MessengerInterface& messenger,
               cmlb::infrastructure::system::SystemMetrics& metrics,
               std::chrono::steady_clock::time_point bot_start_time) noexcept;

    /// Sends the current status view for @p request.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> execute(StatusRequest request);

private:
    cmlb::infrastructure::persistence::TaskRepository& tasks_;
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    cmlb::infrastructure::system::SystemMetrics& metrics_;
    std::chrono::steady_clock::time_point bot_start_time_;
};

} // namespace cmlb::application
