// ---------------------------------------------------------------------------
// show_stats.cpp — ShowStats use case implementation.
// ---------------------------------------------------------------------------

#include <chrono>
#include <string_view>

#include <cmlb/application/show_stats.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace system_ns = cmlb::infrastructure::system;

namespace {

asio::awaitable<void> add_backend_stats(std::string_view label,
                                        download_ns::DownloaderInterface& downloader,
                                        StatsReport& report) {
    auto stats = co_await downloader.global_stats();
    if (!stats) {
        cmlb::core::Logger::warn(
            "show_stats: {} global_stats failed: {}", label, stats.error().message);
        report.unavailable_downloaders.emplace_back(label);
        co_return;
    }
    report.active_downloads += stats->active_count;
}

} // namespace

ShowStats::ShowStats(download_ns::DownloaderInterface& aria2,
                     download_ns::DownloaderInterface& qbit,
                     system_ns::SystemMetrics& metrics,
                     std::chrono::steady_clock::time_point bot_start_time) noexcept
    : aria2_{aria2}, qbit_{qbit}, metrics_{metrics}, bot_start_time_{bot_start_time} {
}

asio::awaitable<cmlb::core::Result<StatsReport>> ShowStats::execute(StatsRequest) {
    StatsReport report;
    report.metrics = metrics_.snapshot();
    report.bot_uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - bot_start_time_);

    co_await add_backend_stats("aria2", aria2_, report);
    co_await add_backend_stats("qbittorrent", qbit_, report);

    co_return report;
}

} // namespace cmlb::application
