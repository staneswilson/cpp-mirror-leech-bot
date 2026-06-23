// ---------------------------------------------------------------------------
// show_status.cpp — ShowStatus use case implementation.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <cmlb/application/show_status.hpp>
#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/task.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace persistence_ns = cmlb::infrastructure::persistence;
namespace system_ns = cmlb::infrastructure::system;
namespace tg_ns = cmlb::infrastructure::telegram;

namespace {

[[nodiscard]] bool visible_to_request(const cmlb::domain::Task& task,
                                      const StatusRequest& request) noexcept {
    if (task.metadata().chat != request.chat) {
        return false;
    }
    return request.include_all_users || task.metadata().user == request.user;
}

[[nodiscard]] download_ns::DownloaderInterface* downloader_for(
    cmlb::domain::DownloaderKind kind,
    download_ns::DownloaderInterface& aria2,
    download_ns::DownloaderInterface& qbit) noexcept {
    switch (kind) {
    case cmlb::domain::DownloaderKind::Aria2:
        return &aria2;
    case cmlb::domain::DownloaderKind::Qbittorrent:
        return &qbit;
    case cmlb::domain::DownloaderKind::None:
        return nullptr;
    }
    return nullptr;
}

[[nodiscard]] std::string render_metrics_footer(const system_ns::SystemSnapshot& metrics,
                                                std::chrono::seconds bot_uptime) {
    return fmt::format("<b>CPU:</b> <code>{:.1f}%</code> | "
                       "<b>RAM:</b> <code>{}/{}</code> | "
                       "<b>Disk:</b> <code>{}/{}</code>\n"
                       "<b>Bot uptime:</b> <code>{}</code>",
                       metrics.cpu_usage_percent,
                       cmlb::core::format_bytes(metrics.ram_used_bytes),
                       cmlb::core::format_bytes(metrics.ram_total_bytes),
                       cmlb::core::format_bytes(metrics.disk_used_bytes),
                       cmlb::core::format_bytes(metrics.disk_total_bytes),
                       cmlb::core::format_duration(bot_uptime));
}

[[nodiscard]] std::string render_no_active_tasks(const system_ns::SystemSnapshot& metrics,
                                                 std::chrono::seconds bot_uptime) {
    return "<b>No active tasks.</b>\n\n" + render_metrics_footer(metrics, bot_uptime);
}

[[nodiscard]] std::string render_downloader_status(const download_ns::DownloadStatus& status,
                                                   std::size_t index) {
    const double fraction = (status.total_bytes > 0) ? static_cast<double>(status.downloaded_bytes)
                                                           / static_cast<double>(status.total_bytes)
                                                     : 0.0;
    const std::string name =
        cmlb::core::escape_html(status.name.empty() ? std::string_view{"(unnamed download)"}
                                                    : std::string_view{status.name});

    return fmt::format("<b>{}.</b> <code>{}</code>\n"
                       "{} <code>{}</code>\n"
                       "<b>Size:</b> <code>{} / {}</code>\n"
                       "<b>Rate:</b> <code>{}</code> | <b>ETA:</b> <code>{}</code>\n"
                       "<b>State:</b> <code>{}</code>",
                       index,
                       name,
                       cmlb::core::render_progress_bar(fraction),
                       cmlb::core::format_percent(fraction),
                       cmlb::core::format_bytes(status.downloaded_bytes),
                       cmlb::core::format_bytes(status.total_bytes),
                       cmlb::core::format_rate(status.download_speed_bps),
                       cmlb::core::format_eta(status.eta),
                       download_ns::to_string(status.state));
}

[[nodiscard]] std::string render_status_list(std::span<const download_ns::DownloadStatus> active,
                                             const system_ns::SystemSnapshot& metrics,
                                             std::chrono::seconds bot_uptime) {
    if (active.empty()) {
        return render_no_active_tasks(metrics, bot_uptime);
    }

    std::string out;
    out.reserve(256 * active.size());

    constexpr std::size_t kMaxRenderedTasks = 10;
    const std::size_t shown = std::min(active.size(), kMaxRenderedTasks);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            out.append("\n\n");
        }
        out.append(render_downloader_status(active[i], i + 1));
    }
    if (active.size() > shown) {
        out.append(fmt::format("\n\n<i>... and {} more task(s)</i>", active.size() - shown));
    }
    out.append("\n\n");
    out.append(render_metrics_footer(metrics, bot_uptime));
    return out;
}

[[nodiscard]] std::string render_task_without_downloader(const cmlb::domain::Task& task,
                                                         const system_ns::SystemSnapshot& metrics,
                                                         std::chrono::seconds bot_uptime) {
    return fmt::format(
        "<b>Task:</b> <code>{}</code>\n"
        "<b>State:</b> <code>{}</code>\n"
        "<b>Source:</b> <code>{}</code>\n\n{}",
        task.metadata().id.value(),
        cmlb::domain::to_string(task.state()),
        cmlb::core::escape_html(cmlb::core::truncate_for_display(task.metadata().source_url, 200)),
        render_metrics_footer(metrics, bot_uptime));
}

} // namespace

ShowStatus::ShowStatus(persistence_ns::TaskRepository& tasks,
                       download_ns::DownloaderInterface& aria2,
                       download_ns::DownloaderInterface& qbit,
                       tg_ns::MessengerInterface& messenger,
                       system_ns::SystemMetrics& metrics,
                       std::chrono::steady_clock::time_point bot_start_time) noexcept
    : tasks_{tasks},
      aria2_{aria2},
      qbit_{qbit},
      messenger_{messenger},
      metrics_{metrics},
      bot_start_time_{bot_start_time} {
}

asio::awaitable<cmlb::core::Result<void>> ShowStatus::execute(StatusRequest request) {
    cmlb::core::Logger::debug("show_status: user={} chat={} task_id={}",
                              request.user.value(),
                              request.chat.value(),
                              request.task_id ? request.task_id->value() : std::string{"(all)"});

    const auto snapshot = metrics_.snapshot();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - bot_start_time_);

    if (request.task_id.has_value()) {
        auto loaded = co_await tasks_.find(*request.task_id);
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        if (!loaded->has_value()) {
            const std::string body = fmt::format("<b>Status:</b> task <code>{}</code> not found.",
                                                 request.task_id->value());
            (void)co_await messenger_.send_html(request.chat, body);
            co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound, "status: task not found");
        }

        const cmlb::domain::Task& task = **loaded;
        if (!visible_to_request(task, request)) {
            cmlb::core::Logger::warn("show_status: denied task={} task_chat={} request_chat={}",
                                     task.metadata().id.value(),
                                     task.metadata().chat.value(),
                                     request.chat.value());
            co_return cmlb::core::error(cmlb::core::ErrorCode::PermissionDenied,
                                        "status: task is not visible to this requester");
        }

        const auto did = task.downloader_id();
        download_ns::DownloaderInterface* downloader =
            downloader_for(task.downloader_kind(), aria2_, qbit_);
        if (!did.has_value() || downloader == nullptr) {
            auto sent = co_await messenger_.send_html(
                request.chat, render_task_without_downloader(task, snapshot, uptime));
            if (!sent) {
                co_return std::unexpected(sent.error());
            }
            co_return cmlb::core::Result<void>{};
        }

        auto status = co_await downloader->status(*did);
        if (!status) {
            cmlb::core::Logger::warn("show_status: downloader status failed task={} gid={}: {}",
                                     task.metadata().id.value(),
                                     did->value(),
                                     status.error().message);
            co_return std::unexpected(status.error());
        }

        auto sent = co_await messenger_.send_html(
            request.chat,
            render_status_list(
                std::span<const download_ns::DownloadStatus>{&*status, 1}, snapshot, uptime));
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
        co_return cmlb::core::Result<void>{};
    }

    auto incomplete = co_await tasks_.incomplete();
    if (!incomplete) {
        co_return std::unexpected(incomplete.error());
    }

    std::vector<download_ns::DownloadStatus> active;
    active.reserve(incomplete->size());
    for (const auto& task : *incomplete) {
        if (!visible_to_request(task, request)) {
            continue;
        }
        const auto did = task.downloader_id();
        download_ns::DownloaderInterface* downloader =
            downloader_for(task.downloader_kind(), aria2_, qbit_);
        if (!did.has_value() || downloader == nullptr) {
            continue;
        }
        auto status = co_await downloader->status(*did);
        if (!status) {
            cmlb::core::Logger::warn("show_status: skipping task={} gid={} after status error: {}",
                                     task.metadata().id.value(),
                                     did->value(),
                                     status.error().message);
            continue;
        }
        active.push_back(std::move(*status));
    }

    auto sent =
        co_await messenger_.send_html(request.chat, render_status_list(active, snapshot, uptime));
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    co_return cmlb::core::Result<void>{};
}

} // namespace cmlb::application
