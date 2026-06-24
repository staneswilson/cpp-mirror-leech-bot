#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>

/// @file html_renderer.hpp
/// @brief Single source for every user-visible HTML string the bot emits.
///
/// All methods are `static` and `[[nodiscard]]`. No state is held; the renderer
/// is purely a name-spaced bag of pure functions over plain data.
///
/// Output uses Telegram HTML that TDLib parses into native `formattedText`
/// entities via `parseTextEntities`. Keep composition here so every reply
/// has one consistent, parseable voice.

namespace cmlb::presentation {

class HtmlRenderer {
public:
    HtmlRenderer() = delete;
    HtmlRenderer(const HtmlRenderer&) = delete;
    HtmlRenderer& operator=(const HtmlRenderer&) = delete;
    HtmlRenderer(HtmlRenderer&&) = delete;
    HtmlRenderer& operator=(HtmlRenderer&&) = delete;

    /// Renders a single task's progress block — name, size, percent, rate, ETA.
    [[nodiscard]] static std::string render_task_status(
        const cmlb::domain::Task& task,
        const cmlb::infrastructure::download::DownloadStatus& status);

    /// Renders the full periodic status message: each active task followed by
    /// a one-line system metrics footer.
    [[nodiscard]] static std::string render_status(
        std::span<const cmlb::infrastructure::download::DownloadStatus> active,
        const cmlb::infrastructure::system::SystemSnapshot& metrics,
        std::chrono::seconds bot_uptime);

    /// Renders the placeholder message shown when there are no active tasks.
    [[nodiscard]] static std::string render_no_active_tasks(
        const cmlb::infrastructure::system::SystemSnapshot& metrics,
        std::chrono::seconds bot_uptime);

    /// Renders the `/stats` command output (host resources + bot uptime).
    [[nodiscard]] static std::string render_stats(
        const cmlb::infrastructure::system::SystemSnapshot& metrics,
        std::chrono::seconds bot_uptime,
        int active_downloads,
        std::span<const std::string> unavailable_downloaders = {});

    /// Description of a single command for the `/help` listing.
    struct CommandDescription {
        std::string name;
        std::string description;
        cmlb::domain::Permission permission;
    };

    /// Renders the `/help` output from a flat list of commands.
    [[nodiscard]] static std::string render_help(std::span<const CommandDescription> commands);

    /// Renders the `/rss list` output with capped, readable feed entries.
    [[nodiscard]] static std::string render_rss_subscriptions(
        std::span<const cmlb::infrastructure::persistence::RssFeed> feeds);

    /// Renders the `/settings` panel for the per-user preferences.
    [[nodiscard]] static std::string render_user_settings(
        const cmlb::infrastructure::persistence::UserSettingsRecord& settings);

    /// Renders the `/botsettings` panel.
    [[nodiscard]] static std::string render_bot_settings(
        const cmlb::infrastructure::persistence::BotSettingsRecord& settings);

    /// Renders a friendly error reply quoting the task name and a
    /// human-readable label for the error code. The raw enum name is kept
    /// out of user-facing strings — it lives in the log.
    [[nodiscard]] static std::string render_error(std::string_view task_name,
                                                  const cmlb::core::AppError& error);

    /// Renders a structured failure block for use-case status messages.
    /// @p stage describes what was happening when the failure occurred
    /// ("download", "upload", ...). The message body is sanitized before
    /// HTML composition.
    [[nodiscard]] static std::string render_failure(std::string_view stage,
                                                    const cmlb::core::AppError& error);

    /// Renders the `/start` greeting block. Falls back to a generic
    /// greeting when @p user_first_name is empty.
    [[nodiscard]] static std::string render_greeting(std::string_view user_first_name);

    /// Renders a premium section title using Telegram rich text entities.
    [[nodiscard]] static std::string render_heading(std::string_view text);

    /// Renders escaped inline code.
    [[nodiscard]] static std::string render_code(std::string_view text);

    /// Renders escaped supporting detail as a Telegram block quote.
    [[nodiscard]] static std::string render_quote(std::string_view text);

    /// Renders a command usage hint such as `/mirror <url>`.
    [[nodiscard]] static std::string render_usage(std::string_view command,
                                                  std::string_view syntax);

    /// Renders a compact success block with an optional quoted detail.
    [[nodiscard]] static std::string render_success(std::string_view title,
                                                    std::string_view detail = {});

    /// Renders a compact operational notice with an optional quoted detail.
    [[nodiscard]] static std::string render_notice(std::string_view title,
                                                   std::string_view detail = {});

    /// Returns a short, human-readable label for @p code, suitable for
    /// user-facing copy. Never returns a raw enumerator name.
    [[nodiscard]] static std::string_view friendly_label(cmlb::core::ErrorCode code) noexcept;

    /// Returns a UTF-8-boundary-safe display copy of @p text, never longer
    /// than @p max_bytes. If the input is longer, trailing bytes are
    /// trimmed at the closest UTF-8 codepoint boundary and replaced with
    /// `...`. Safe for placement inside `<code>` / `<pre>` after escaping.
    [[nodiscard]] static std::string truncate_for_display(std::string_view text,
                                                          std::size_t max_bytes);

    /// Escapes Telegram-significant HTML characters (`&`, `<`, `>`). Exposed
    /// so other presentation-layer modules don't reinvent it.
    [[nodiscard]] static std::string escape_html(std::string_view text);
};

} // namespace cmlb::presentation
