#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>

/// @file show_stats.hpp
/// @brief ShowStats use case — collect host and downloader statistics for `/stats`.

namespace cmlb::application {

/// Request DTO for `ShowStats::execute`.
struct StatsRequest {};

/// Application-layer report consumed by the presentation renderer.
struct StatsReport {
    cmlb::infrastructure::system::SystemSnapshot metrics;
    std::chrono::seconds bot_uptime{0};
    int active_downloads{0};
    std::vector<std::string> unavailable_downloaders;
};

/// Collects system metrics and backend-wide downloader counts for `/stats`.
class ShowStats {
public:
    ShowStats(cmlb::infrastructure::download::DownloaderInterface& aria2,
              cmlb::infrastructure::download::DownloaderInterface& qbit,
              cmlb::infrastructure::system::SystemMetrics& metrics,
              std::chrono::steady_clock::time_point bot_start_time) noexcept;

    /// Returns the current stats report.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<StatsReport>> execute(
        StatsRequest request);

private:
    cmlb::infrastructure::download::DownloaderInterface& aria2_;
    cmlb::infrastructure::download::DownloaderInterface& qbit_;
    cmlb::infrastructure::system::SystemMetrics& metrics_;
    std::chrono::steady_clock::time_point bot_start_time_;
};

} // namespace cmlb::application
