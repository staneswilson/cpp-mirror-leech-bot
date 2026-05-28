// ---------------------------------------------------------------------------
// mirror_url_test.cpp - unit tests for MirrorUrl orchestration.
// ---------------------------------------------------------------------------

#include "in_memory_task_repository.hpp"
#include "in_memory_user_settings_repository.hpp"
#include "stub_downloader.hpp"
#include "stub_messenger.hpp"
#include "stub_uploader.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/mirror_url.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/presentation/progress_renderer.hpp>

namespace asio = boost::asio;

using cmlb::application::ActiveTaskRegistry;
using cmlb::application::MirrorRequest;
using cmlb::application::MirrorUrl;
using cmlb::core::ErrorCode;
using cmlb::domain::ChatId;
using cmlb::domain::Gid;
using cmlb::domain::MessageId;
using cmlb::domain::UserId;
using cmlb::infrastructure::download::DownloadState;
using cmlb::infrastructure::download::DownloadStatus;
using cmlb::test_support::InMemoryTaskRepository;
using cmlb::test_support::InMemoryUserSettingsRepository;
using cmlb::test_support::StubDownloader;
using cmlb::test_support::StubMessenger;
using cmlb::test_support::StubUploader;

namespace {

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

struct Fixture {
    cmlb::core::Executor executor{std::size_t{1}};
    StubDownloader aria2;
    StubDownloader qbit;
    StubUploader gdrive{"gdrive"};
    StubUploader rclone{"rclone"};
    InMemoryTaskRepository tasks;
    InMemoryUserSettingsRepository settings;
    StubMessenger messenger;
    cmlb::infrastructure::system::SystemMetrics metrics;
    cmlb::presentation::ProgressRenderer progress_renderer{messenger,
                                                           metrics,
                                                           std::chrono::steady_clock::now(),
                                                           executor.get_executor(),
                                                           std::chrono::milliseconds{0}};
    ActiveTaskRegistry active_tasks;

    MirrorUrl make() {
        return MirrorUrl{aria2,
                         qbit,
                         gdrive,
                         rclone,
                         tasks,
                         settings,
                         messenger,
                         progress_renderer,
                         executor,
                         active_tasks,
                         4};
    }
};

MirrorRequest sample_request(std::string url = "https://example.com/file.iso") {
    return MirrorRequest{
        .url = std::move(url),
        .user = UserId{42},
        .chat = ChatId{-100123},
        .source_message = MessageId{7},
        .use_qbittorrent = false,
        .override_destination = std::nullopt,
    };
}

DownloadStatus complete_status() {
    DownloadStatus s;
    s.id = Gid{std::string{"stub-gid"}};
    s.name = "file.iso";
    s.state = DownloadState::Complete;
    s.total_bytes = 1024;
    s.downloaded_bytes = 1024;
    s.save_path = std::filesystem::path{"/tmp/x"};
    s.files = {std::filesystem::path{"/tmp/x/file.iso"}};
    return s;
}

} // namespace

TEST_CASE("MirrorUrl rejects an empty URL with InvalidArgument", "[application][mirror_url]") {
    Fixture fix;
    auto uc = fix.make();
    asio::io_context ctx;

    auto req = sample_request("");
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await uc.execute(req);
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(fix.tasks.size() == 0);
}

TEST_CASE("MirrorUrl happy path persists task and uploads via gdrive",
          "[application][mirror_url]") {
    Fixture fix;
    fix.aria2.set_next_gid(Gid{std::string{"abc-gid"}});
    fix.aria2.push_status(complete_status());

    auto uc = fix.make();
    asio::io_context ctx;

    auto req = sample_request();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await uc.execute(req);
    });

    REQUIRE(result.has_value());
    CHECK(fix.tasks.size() == 1);
    CHECK(fix.aria2.add_calls().size() == 1);
    CHECK(fix.aria2.add_calls()[0].uri == "https://example.com/file.iso");
    // Pipelined path: each file in DownloadStatus.files is spawned as an
    // individual upload_file rather than a single upload_directory.
    CHECK(fix.gdrive.file_calls().size() == 1);
    CHECK(fix.gdrive.directory_calls().empty());
    CHECK(fix.rclone.file_calls().empty());
    // Initial "Queued" send + ProgressRenderer's first send-with-keyboard
    // (when the chat has no cached message yet).
    CHECK_FALSE(fix.messenger.sends().empty());
    CHECK_FALSE(fix.messenger.edits().empty());
}

TEST_CASE("MirrorUrl marks task Failed on downloader error", "[application][mirror_url]") {
    Fixture fix;
    DownloadStatus err;
    err.id = Gid{std::string{"stub-gid"}};
    err.state = DownloadState::Error;
    err.error_message = "network died";
    fix.aria2.push_status(err);

    auto uc = fix.make();
    asio::io_context ctx;

    auto req = sample_request();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await uc.execute(req);
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(fix.tasks.size() == 1);
    // Upload path was never touched.
    CHECK(fix.gdrive.directory_calls().empty());
    CHECK(fix.gdrive.file_calls().empty());
}

TEST_CASE("MirrorUrl marks task Failed when uploader returns an error",
          "[application][mirror_url]") {
    Fixture fix;
    fix.aria2.push_status(complete_status());
    fix.gdrive.set_error(cmlb::core::AppError{ErrorCode::GoogleDriveApi, "quota exceeded"});

    auto uc = fix.make();
    asio::io_context ctx;

    auto req = sample_request();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await uc.execute(req);
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(fix.tasks.size() == 1);
    // Pipelined path attempts upload_file per produced file.
    CHECK(fix.gdrive.file_calls().size() >= 1);
}

TEST_CASE("MirrorUrl routes through qBittorrent when use_qbittorrent is set",
          "[application][mirror_url]") {
    Fixture fix;
    fix.qbit.set_next_gid(Gid{std::string{"qb-1"}});
    fix.qbit.push_status(complete_status());

    auto uc = fix.make();
    asio::io_context ctx;

    auto req = sample_request();
    req.use_qbittorrent = true;
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await uc.execute(req);
    });

    REQUIRE(result.has_value());
    CHECK(fix.qbit.add_calls().size() == 1);
    CHECK(fix.aria2.add_calls().empty());
}
