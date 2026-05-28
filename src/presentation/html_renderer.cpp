// ---------------------------------------------------------------------------
// html_renderer.cpp
//
// Single source for every user-visible HTML string. All formatting helpers
// (bytes, durations, percents, progress bars) are imported from
// `cmlb::core::formatting`; no local re-implementations.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <cmlb/core/formatting.hpp>
#include <cmlb/presentation/html_renderer.hpp>

namespace cmlb::presentation {

using cmlb::core::format_bytes;
using cmlb::core::format_duration;
using cmlb::core::format_eta;
using cmlb::core::format_percent;
using cmlb::core::format_rate;
using cmlb::core::friendly_error_label;
using cmlb::core::render_progress_bar;
using cmlb::core::truncate_for_display;

namespace {

[[nodiscard]] std::string_view kind_label(cmlb::domain::TaskKind kind) noexcept {
    switch (kind) {
    case cmlb::domain::TaskKind::Mirror:
        return "Mirror";
    case cmlb::domain::TaskKind::Leech:
        return "Leech";
    case cmlb::domain::TaskKind::Clone:
        return "Clone";
    }
    return "Task";
}

[[nodiscard]] std::string render_metrics_footer(
    const cmlb::infrastructure::system::SystemSnapshot& metrics, std::chrono::seconds bot_uptime) {
    return fmt::format("<b>CPU:</b> <code>{:.1f}%</code> | "
                       "<b>RAM:</b> <code>{}/{}</code> | "
                       "<b>Disk:</b> <code>{}/{}</code>\n"
                       "<b>Bot uptime:</b> <code>{}</code>",
                       metrics.cpu_usage_percent,
                       format_bytes(metrics.ram_used_bytes),
                       format_bytes(metrics.ram_total_bytes),
                       format_bytes(metrics.disk_used_bytes),
                       format_bytes(metrics.disk_total_bytes),
                       format_duration(bot_uptime));
}

} // namespace

std::string HtmlRenderer::escape_html(std::string_view text) {
    return cmlb::core::escape_html(text);
}

std::string HtmlRenderer::render_task_status(
    const cmlb::domain::Task& task, const cmlb::infrastructure::download::DownloadStatus& status) {
    const double fraction = (status.total_bytes > 0) ? static_cast<double>(status.downloaded_bytes)
                                                           / static_cast<double>(status.total_bytes)
                                                     : 0.0;

    const std::string name =
        escape_html(status.name.empty() ? task.metadata().source_url : status.name);

    return fmt::format("<b>{}:</b> <code>{}</code>\n"
                       "{} <code>{}</code>\n"
                       "<b>Size:</b> <code>{} / {}</code>\n"
                       "<b>Rate:</b> <code>{}</code> | <b>ETA:</b> <code>{}</code>\n"
                       "<b>State:</b> <code>{}</code>",
                       kind_label(task.metadata().kind),
                       name,
                       render_progress_bar(fraction),
                       format_percent(fraction),
                       format_bytes(status.downloaded_bytes),
                       format_bytes(status.total_bytes),
                       format_rate(status.download_speed_bps),
                       format_eta(status.eta),
                       cmlb::infrastructure::download::to_string(status.state));
}

std::string HtmlRenderer::render_status(
    std::span<const cmlb::infrastructure::download::DownloadStatus> active,
    const cmlb::infrastructure::system::SystemSnapshot& metrics,
    std::chrono::seconds bot_uptime) {
    if (active.empty()) {
        return render_no_active_tasks(metrics, bot_uptime);
    }

    std::string out;
    out.reserve(256 * active.size());

    constexpr std::size_t kMaxRenderedTasks = 10;
    const std::size_t shown = std::min(active.size(), kMaxRenderedTasks);

    for (std::size_t i = 0; i < shown; ++i) {
        const auto& s = active[i];
        const double fraction = (s.total_bytes > 0) ? static_cast<double>(s.downloaded_bytes)
                                                          / static_cast<double>(s.total_bytes)
                                                    : 0.0;
        out.append(fmt::format("<b>{}.</b> <code>{}</code>\n"
                               "{} <code>{}</code>\n"
                               "<b>Size:</b> <code>{} / {}</code>\n"
                               "<b>Rate:</b> <code>{}</code> | <b>ETA:</b> <code>{}</code>\n"
                               "<b>State:</b> <code>{}</code>\n\n",
                               i + 1,
                               escape_html(s.name),
                               render_progress_bar(fraction),
                               format_percent(fraction),
                               format_bytes(s.downloaded_bytes),
                               format_bytes(s.total_bytes),
                               format_rate(s.download_speed_bps),
                               format_eta(s.eta),
                               cmlb::infrastructure::download::to_string(s.state)));
    }

    if (active.size() > shown) {
        out.append(fmt::format("<i>... and {} more task(s)</i>\n\n", active.size() - shown));
    }

    out.append(render_metrics_footer(metrics, bot_uptime));
    return out;
}

std::string HtmlRenderer::render_no_active_tasks(
    const cmlb::infrastructure::system::SystemSnapshot& metrics, std::chrono::seconds bot_uptime) {
    std::string out{"<b>No active tasks.</b>\n\n"};
    out.append(render_metrics_footer(metrics, bot_uptime));
    return out;
}

std::string HtmlRenderer::render_stats(const cmlb::infrastructure::system::SystemSnapshot& metrics,
                                       std::chrono::seconds bot_uptime,
                                       int active_downloads) {
    return fmt::format("<b>Bot Statistics</b>\n"
                       "<b>Active downloads:</b> <code>{}</code>\n"
                       "<b>Bot uptime:</b> <code>{}</code>\n"
                       "<b>System uptime:</b> <code>{}</code>\n"
                       "<b>CPU:</b> <code>{:.1f}%</code>\n"
                       "<b>RAM:</b> <code>{} / {}</code>\n"
                       "<b>Disk:</b> <code>{} / {}</code>\n"
                       "<b>Load avg:</b> <code>{:.2f} / {:.2f} / {:.2f}</code>",
                       active_downloads,
                       format_duration(bot_uptime),
                       format_duration(metrics.system_uptime),
                       metrics.cpu_usage_percent,
                       format_bytes(metrics.ram_used_bytes),
                       format_bytes(metrics.ram_total_bytes),
                       format_bytes(metrics.disk_used_bytes),
                       format_bytes(metrics.disk_total_bytes),
                       metrics.load_average_1m,
                       metrics.load_average_5m,
                       metrics.load_average_15m);
}

std::string HtmlRenderer::render_help(std::span<const CommandDescription> commands) {
    std::string out{"<b>Available commands</b>\n"};
    for (const auto& cmd : commands) {
        out.append(fmt::format(
            "<code>/{}</code> <i>({})</i>", cmd.name, cmlb::domain::to_string(cmd.permission)));
        if (!cmd.description.empty()) {
            out.push_back(' ');
            out.append(escape_html(cmd.description));
        }
        out.push_back('\n');
    }
    return out;
}

std::string HtmlRenderer::render_user_settings(
    const cmlb::infrastructure::persistence::UserSettingsRecord& settings) {
    return fmt::format("<b>User Settings</b>\n"
                       "<b>User id:</b> <code>{}</code>\n"
                       "<b>Leech destination:</b> <code>{}</code>\n"
                       "<b>Mirror destination:</b> <code>{}</code>\n"
                       "<b>Upload as document:</b> <code>{}</code>\n"
                       "<b>Rclone remote:</b> <code>{}</code>\n"
                       "<b>GDrive folder id:</b> <code>{}</code>\n"
                       "<b>Default thumbnail:</b> <code>{}</code>",
                       settings.user_id.value(),
                       cmlb::domain::to_string(settings.leech_destination),
                       cmlb::domain::to_string(settings.mirror_destination),
                       settings.upload_as_document ? "yes" : "no",
                       settings.rclone_remote.value_or("(none)"),
                       settings.gdrive_folder_id.value_or("(none)"),
                       settings.default_thumb_path.value_or("(none)"));
}

namespace {

/// Renders an id list inline as `[<id>, <id>, ...]`, capped at 8 entries with
/// a trailing `+N more` if exceeded. Operators with sprawling allowlists still
/// see *something* concrete instead of just a count.
[[nodiscard]] std::string render_id_list(std::span<const std::int64_t> ids) {
    if (ids.empty())
        return "<i>(none)</i>";
    constexpr std::size_t kMaxInline = 8;
    const std::size_t shown = std::min(ids.size(), kMaxInline);
    std::string out;
    out.reserve(16 * shown);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0)
            out.append(", ");
        out.append("<code>");
        out.append(std::to_string(ids[i]));
        out.append("</code>");
    }
    if (ids.size() > shown) {
        out.append(fmt::format(" <i>(+{} more)</i>", ids.size() - shown));
    }
    return out;
}

} // namespace

std::string HtmlRenderer::render_bot_settings(
    const cmlb::infrastructure::persistence::BotSettingsRecord& settings) {
    return fmt::format("<b>Bot Settings</b>\n"
                       "<b>Owner id:</b> <code>{}</code>\n"
                       "<b>Sudo users ({}):</b> {}\n"
                       "<b>Authorized chats ({}):</b> {}\n"
                       "<b>Download dir:</b> <code>{}</code>\n"
                       "<b>Leech split size:</b> <code>{}</code>\n"
                       "<b>Upload limit:</b> <code>{}</code>\n"
                       "<b>Status update interval:</b> <code>{} ms</code>\n"
                       "<b>RSS poll interval:</b> <code>{} ms</code>",
                       settings.owner_id,
                       settings.sudo_users.size(),
                       render_id_list(settings.sudo_users),
                       settings.authorized_chats.size(),
                       render_id_list(settings.authorized_chats),
                       escape_html(settings.download_dir.string()),
                       format_bytes(settings.leech_split_size),
                       settings.upload_limit_bytes == 0 ? std::string{"unlimited"}
                                                        : format_bytes(settings.upload_limit_bytes),
                       settings.status_update_interval.count(),
                       settings.rss_poll_interval.count());
}

std::string_view HtmlRenderer::friendly_label(cmlb::core::ErrorCode code) noexcept {
    return friendly_error_label(code);
}

std::string HtmlRenderer::truncate_for_display(std::string_view text, std::size_t max_bytes) {
    return cmlb::core::truncate_for_display(text, max_bytes);
}

std::string HtmlRenderer::render_failure(std::string_view stage,
                                         const cmlb::core::AppError& error) {
    // Strip internal context prefixes that may have been prepended by the
    // use case (e.g. "downloader add_uri: …", "upload: …"). The stage
    // already conveys what was happening; duplicating it in the detail
    // adds noise.
    std::string_view detail{error.message};
    constexpr std::array<std::string_view, 5> kInternalPrefixes{{
        "downloader add_uri: ",
        "downloader status: ",
        "download error: ",
        "upload: ",
        "mark_failed: ",
    }};
    for (const auto& prefix : kInternalPrefixes) {
        if (detail.starts_with(prefix)) {
            detail.remove_prefix(prefix.size());
            break;
        }
    }
    // Final cap so a malicious or runaway upstream error message can't
    // produce a multi-kB Telegram payload.
    const std::string trimmed = truncate_for_display(detail, 512);
    return fmt::format("<b>Failed</b> ({})\n"
                       "<b>Reason:</b> {}\n"
                       "<pre>{}</pre>",
                       escape_html(stage),
                       escape_html(friendly_label(error.code)),
                       escape_html(trimmed));
}

std::string HtmlRenderer::render_error(std::string_view task_name,
                                       const cmlb::core::AppError& error) {
    return fmt::format("<b>Error</b>\n"
                       "<b>Task:</b> <code>{}</code>\n"
                       "<b>Reason:</b> {}\n"
                       "<pre>{}</pre>",
                       escape_html(task_name),
                       escape_html(friendly_label(error.code)),
                       escape_html(truncate_for_display(error.message, 512)));
}

std::string HtmlRenderer::render_greeting(std::string_view user_first_name) {
    const std::string who =
        user_first_name.empty() ? std::string{"there"} : escape_html(user_first_name);
    return fmt::format("<b>Hello, {}!</b>\n"
                       "I'm <b>cmlb</b> - a mirror/leech bot.\n\n"
                       "Send a URL, magnet link, or torrent file with <code>/mirror</code> "
                       "to upload to cloud storage, or <code>/leech</code> to receive it "
                       "back in this chat.\n\n"
                       "Type <code>/help</code> for the full command list and "
                       "<code>/settings</code> to configure your preferences.",
                       who);
}

} // namespace cmlb::presentation
