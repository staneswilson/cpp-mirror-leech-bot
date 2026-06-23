// ---------------------------------------------------------------------------
// html_renderer_test.cpp
//
// Smoke tests for `cmlb::presentation::HtmlRenderer`. Output is golden-string
// adjacent - we assert structural fragments (HTML tags, key labels, escaped
// values) rather than exact byte-for-byte equality so cosmetic format tweaks
// do not break the suite.
// ---------------------------------------------------------------------------

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/presentation/html_renderer.hpp>

using Catch::Matchers::ContainsSubstring;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::Permission;
using cmlb::domain::TaskKind;
using cmlb::domain::TaskMetadata;
using cmlb::domain::UserId;
using cmlb::infrastructure::download::DownloadState;
using cmlb::infrastructure::download::DownloadStatus;
using cmlb::infrastructure::system::SystemSnapshot;
using cmlb::presentation::HtmlRenderer;

namespace {

[[nodiscard]] cmlb::domain::Task make_task() {
    TaskMetadata md{
        .id = cmlb::domain::TaskId{"task-1"},
        .user = UserId{42},
        .chat = ChatId{-1001},
        .status_message = MessageId{99},
        .kind = TaskKind::Mirror,
        .source_url = "https://example.com/file.iso",
        .created_at = {},
        .updated_at = {},
    };
    return cmlb::domain::Task{md};
}

[[nodiscard]] DownloadStatus make_download_status() {
    DownloadStatus s;
    s.id = cmlb::domain::Gid{"abcdef0123456789"};
    s.name = "Big File.iso";
    s.state = DownloadState::Downloading;
    s.total_bytes = 100 * 1024 * 1024;
    s.downloaded_bytes = 25 * 1024 * 1024;
    s.download_speed_bps = 5 * 1024 * 1024;
    s.eta = std::chrono::seconds{30};
    return s;
}

[[nodiscard]] SystemSnapshot make_snapshot() {
    SystemSnapshot s;
    s.cpu_usage_percent = 42.5;
    s.ram_used_bytes = 4LL * 1024 * 1024 * 1024;
    s.ram_total_bytes = 16LL * 1024 * 1024 * 1024;
    s.disk_used_bytes = 50LL * 1024 * 1024 * 1024;
    s.disk_total_bytes = 500LL * 1024 * 1024 * 1024;
    s.bot_uptime = std::chrono::seconds{3600};
    s.system_uptime = std::chrono::seconds{86400};
    s.load_average_1m = 1.23;
    s.load_average_5m = 1.05;
    s.load_average_15m = 0.92;
    return s;
}

} // namespace

TEST_CASE("escape_html escapes the three significant characters", "[presentation][html]") {
    CHECK(HtmlRenderer::escape_html("a < b & c > d") == "a &lt; b &amp; c &gt; d");
    CHECK(HtmlRenderer::escape_html("plain text") == "plain text");
    CHECK(HtmlRenderer::escape_html("") == "");
}

TEST_CASE("render_task_status produces the expected fragments", "[presentation][html]") {
    const auto task = make_task();
    const auto status = make_download_status();
    const auto html = HtmlRenderer::render_task_status(task, status);

    CHECK_THAT(html, ContainsSubstring("<b>Mirror:</b>"));
    CHECK_THAT(html, ContainsSubstring("Big File.iso"));
    CHECK_THAT(html, ContainsSubstring("[###"));
    CHECK_THAT(html, ContainsSubstring("25.0%"));
    CHECK_THAT(html, ContainsSubstring("MiB"));
    CHECK_THAT(html, ContainsSubstring("MiB/s"));
    CHECK_THAT(html, ContainsSubstring("~30s"));
    CHECK_THAT(html, ContainsSubstring("downloading"));
}

TEST_CASE("render_task_status escapes HTML-special characters in the name",
          "[presentation][html]") {
    auto status = make_download_status();
    status.name = "evil<tag>&amp;";
    const auto html = HtmlRenderer::render_task_status(make_task(), status);
    CHECK_THAT(html, ContainsSubstring("evil&lt;tag&gt;&amp;amp;"));
}

TEST_CASE("render_status with empty span returns the no-tasks placeholder",
          "[presentation][html]") {
    const auto snapshot = make_snapshot();
    const auto html = HtmlRenderer::render_status({}, snapshot, std::chrono::seconds{3600});
    CHECK_THAT(html, ContainsSubstring("<b>No active tasks.</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>CPU:</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>Bot uptime:</b>"));
    CHECK_THAT(html, ContainsSubstring("1h 0m 0s"));
}

TEST_CASE("render_status with multiple tasks numbers and counts them", "[presentation][html]") {
    const auto snapshot = make_snapshot();
    std::vector<DownloadStatus> downloads;
    downloads.reserve(3);
    for (int i = 0; i < 3; ++i) {
        auto s = make_download_status();
        s.name = std::string{"file-"} + std::to_string(i) + ".iso";
        downloads.push_back(s);
    }
    const auto html = HtmlRenderer::render_status(downloads, snapshot, std::chrono::seconds{120});
    CHECK_THAT(html, ContainsSubstring("<b>1.</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>2.</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>3.</b>"));
    CHECK_THAT(html, ContainsSubstring("file-0.iso"));
    CHECK_THAT(html, ContainsSubstring("file-2.iso"));
    CHECK_THAT(html, ContainsSubstring("2m 0s"));
}

TEST_CASE("render_status truncates excess tasks beyond the limit", "[presentation][html]") {
    std::vector<DownloadStatus> downloads;
    downloads.reserve(15);
    for (int i = 0; i < 15; ++i) {
        auto s = make_download_status();
        s.name = std::string{"file-"} + std::to_string(i);
        downloads.push_back(s);
    }
    const auto html =
        HtmlRenderer::render_status(downloads, make_snapshot(), std::chrono::seconds{60});
    CHECK_THAT(html, ContainsSubstring("more task(s)"));
}

TEST_CASE("render_no_active_tasks always contains the footer", "[presentation][html]") {
    const auto html =
        HtmlRenderer::render_no_active_tasks(make_snapshot(), std::chrono::seconds{45});
    CHECK_THAT(html, ContainsSubstring("<b>No active tasks.</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>RAM:</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>Disk:</b>"));
    CHECK_THAT(html, ContainsSubstring("45s"));
}

TEST_CASE("render_stats reports active downloads and load averages", "[presentation][html]") {
    const auto html = HtmlRenderer::render_stats(make_snapshot(), std::chrono::seconds{125}, 7);
    CHECK_THAT(html, ContainsSubstring("<b>Bot Statistics</b>"));
    CHECK_THAT(html, ContainsSubstring("<b>Active downloads:</b>"));
    CHECK_THAT(html, ContainsSubstring("<code>7</code>"));
    CHECK_THAT(html, ContainsSubstring("<b>Load avg:</b>"));
    CHECK_THAT(html, ContainsSubstring("1.23"));
    CHECK_THAT(html, ContainsSubstring("0.92"));
    CHECK_THAT(html, ContainsSubstring("2m 5s"));
}

TEST_CASE("render_stats reports degraded downloader backends", "[presentation][html]") {
    const std::array<std::string, 2> unavailable{"aria2", "qbittorrent"};
    const auto html =
        HtmlRenderer::render_stats(make_snapshot(), std::chrono::seconds{125}, 0, unavailable);
    CHECK_THAT(html, ContainsSubstring("<b>Unavailable downloaders:</b>"));
    CHECK_THAT(html, ContainsSubstring("<code>aria2, qbittorrent</code>"));
}

TEST_CASE("render_help renders each command name and permission", "[presentation][html]") {
    const std::array<HtmlRenderer::CommandDescription, 3> cmds{
        HtmlRenderer::CommandDescription{"mirror", "Mirror a URL", Permission::User},
        HtmlRenderer::CommandDescription{"help", "", Permission::Anyone},
        HtmlRenderer::CommandDescription{"log", "Send log", Permission::Owner},
    };
    const auto html = HtmlRenderer::render_help(cmds);
    CHECK_THAT(html, ContainsSubstring("<b>Available commands</b>"));
    CHECK_THAT(html, ContainsSubstring("<code>/mirror</code>"));
    CHECK_THAT(html, ContainsSubstring("<code>/help</code>"));
    CHECK_THAT(html, ContainsSubstring("<code>/log</code>"));
    CHECK_THAT(html, ContainsSubstring("(user)"));
    CHECK_THAT(html, ContainsSubstring("(anyone)"));
    CHECK_THAT(html, ContainsSubstring("(owner)"));
    CHECK_THAT(html, ContainsSubstring("Mirror a URL"));
}

TEST_CASE("render_user_settings renders every key field", "[presentation][html]") {
    cmlb::infrastructure::persistence::UserSettingsRecord rec;
    rec.user_id = UserId{777};
    rec.leech_destination = cmlb::domain::UploadDestination::Telegram;
    rec.mirror_destination = cmlb::domain::UploadDestination::GoogleDrive;
    rec.upload_as_document = true;
    rec.rclone_remote = "drive:";
    rec.gdrive_folder_id = "abc123";

    const auto html = HtmlRenderer::render_user_settings(rec);
    CHECK_THAT(html, ContainsSubstring("<b>User Settings</b>"));
    CHECK_THAT(html, ContainsSubstring("<code>777</code>"));
    CHECK_THAT(html, ContainsSubstring("telegram"));
    CHECK_THAT(html, ContainsSubstring("gdrive"));
    CHECK_THAT(html, ContainsSubstring("yes"));
    CHECK_THAT(html, ContainsSubstring("drive:"));
    CHECK_THAT(html, ContainsSubstring("abc123"));
    CHECK_THAT(html, ContainsSubstring("(none)"));
}

TEST_CASE("render_bot_settings renders the owner id and intervals", "[presentation][html]") {
    cmlb::infrastructure::persistence::BotSettingsRecord rec;
    rec.owner_id = 12345;
    rec.sudo_users = {100, 200};
    rec.authorized_chats = {-1001};
    rec.download_dir = std::filesystem::path{"downloads"};
    rec.leech_split_size = 2'000'000'000;
    rec.upload_limit_bytes = 0;
    rec.status_update_interval = std::chrono::milliseconds{5000};
    rec.rss_poll_interval = std::chrono::milliseconds{60000};

    const auto html = HtmlRenderer::render_bot_settings(rec);
    CHECK_THAT(html, ContainsSubstring("<b>Bot Settings</b>"));
    CHECK_THAT(html, ContainsSubstring("<code>12345</code>"));
    CHECK_THAT(html, ContainsSubstring("unlimited"));
    CHECK_THAT(html, ContainsSubstring("downloads"));
    CHECK_THAT(html, ContainsSubstring("5000 ms"));
    CHECK_THAT(html, ContainsSubstring("60000 ms"));
}

TEST_CASE("render_error reports the task name, code, and message", "[presentation][html]") {
    const cmlb::core::AppError err{cmlb::core::ErrorCode::Network, "connection refused"};
    const auto html = HtmlRenderer::render_error("file.iso", err);
    CHECK_THAT(html, ContainsSubstring("<b>Error</b>"));
    CHECK_THAT(html, ContainsSubstring("file.iso"));
    CHECK_THAT(html, ContainsSubstring("Network"));
    CHECK_THAT(html, ContainsSubstring("connection refused"));
}

TEST_CASE("render_error escapes HTML-special characters in inputs", "[presentation][html]") {
    const cmlb::core::AppError err{cmlb::core::ErrorCode::InvalidArgument, "bad <tag> & value"};
    const auto html = HtmlRenderer::render_error("a<b>c", err);
    CHECK_THAT(html, ContainsSubstring("a&lt;b&gt;c"));
    CHECK_THAT(html, ContainsSubstring("bad &lt;tag&gt; &amp; value"));
}
