// ---------------------------------------------------------------------------
// show_status_test.cpp - unit tests for the /status use case.
// ---------------------------------------------------------------------------

#include "in_memory_task_repository.hpp"
#include "stub_downloader.hpp"
#include "stub_messenger.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/application/show_status.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>

namespace asio = boost::asio;

using Catch::Matchers::ContainsSubstring;
using cmlb::application::ShowStatus;
using cmlb::application::StatusRequest;
using cmlb::domain::ChatId;
using cmlb::domain::DownloaderKind;
using cmlb::domain::Gid;
using cmlb::domain::MessageId;
using cmlb::domain::Task;
using cmlb::domain::TaskId;
using cmlb::domain::TaskKind;
using cmlb::domain::TaskMetadata;
using cmlb::domain::UserId;
using cmlb::infrastructure::download::DownloadState;
using cmlb::infrastructure::download::DownloadStatus;
using cmlb::test_support::InMemoryTaskRepository;
using cmlb::test_support::StubDownloader;
using cmlb::test_support::StubMessenger;

namespace {

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

Task make_task(std::string id,
               UserId user = UserId{42},
               ChatId chat = ChatId{-100123},
               std::string source_url = "https://example.com/file.iso") {
    const auto now = std::chrono::system_clock::now();
    Task task{TaskMetadata{
        .id = TaskId{std::move(id)},
        .user = user,
        .chat = chat,
        .status_message = MessageId{99},
        .kind = TaskKind::Mirror,
        .source_url = std::move(source_url),
        .created_at = now,
        .updated_at = now,
    }};
    REQUIRE(task.start_download().has_value());
    return task;
}

DownloadStatus status_snapshot(std::string gid, std::string name) {
    DownloadStatus s;
    s.id = Gid{std::move(gid)};
    s.name = std::move(name);
    s.state = DownloadState::Downloading;
    s.total_bytes = 1024;
    s.downloaded_bytes = 512;
    s.download_speed_bps = 128;
    s.eta = std::chrono::seconds{4};
    return s;
}

struct Fixture {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    StubMessenger messenger;
    cmlb::infrastructure::system::SystemMetrics metrics;

    ShowStatus make() {
        return ShowStatus{tasks, aria2, qbit, messenger, metrics, std::chrono::steady_clock::now()};
    }
};

} // namespace

TEST_CASE("ShowStatus reports no active tasks when repository is empty",
          "[application][show_status]") {
    Fixture fix;
    auto use_case = fix.make();
    asio::io_context ctx;

    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await use_case.execute(StatusRequest{
            .user = UserId{42},
            .chat = ChatId{-100123},
        });
    });

    REQUIRE(result.has_value());
    const auto sends = fix.messenger.sends();
    REQUIRE(sends.size() == 1);
    CHECK_THAT(sends.front().html, ContainsSubstring("No active tasks"));
}

TEST_CASE("ShowStatus renders active aria2 task status", "[application][show_status]") {
    Fixture fix;
    auto task = make_task("task-aria2");
    task.attach_downloader(Gid{std::string{"aria2-gid"}}, DownloaderKind::Aria2);
    fix.aria2.push_status(status_snapshot("aria2-gid", "debian.iso"));

    asio::io_context ctx;
    auto saved = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await fix.tasks.save(task);
    });
    REQUIRE(saved.has_value());

    auto use_case = fix.make();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await use_case.execute(StatusRequest{
            .user = UserId{42},
            .chat = ChatId{-100123},
        });
    });

    REQUIRE(result.has_value());
    const auto sends = fix.messenger.sends();
    REQUIRE(sends.size() == 1);
    CHECK_THAT(sends.front().html, ContainsSubstring("debian.iso"));
    CHECK_THAT(sends.front().html, ContainsSubstring("50.0%"));
    CHECK_THAT(sends.front().html, ContainsSubstring("downloading"));
}

TEST_CASE("ShowStatus routes qBittorrent tasks to qBittorrent downloader",
          "[application][show_status]") {
    Fixture fix;
    auto task = make_task("task-qbit");
    task.attach_downloader(Gid{std::string{"qbit-hash"}}, DownloaderKind::Qbittorrent);
    fix.qbit.push_status(status_snapshot("qbit-hash", "movie.mkv"));

    asio::io_context ctx;
    auto saved = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await fix.tasks.save(task);
    });
    REQUIRE(saved.has_value());

    auto use_case = fix.make();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await use_case.execute(StatusRequest{
            .user = UserId{42},
            .chat = ChatId{-100123},
        });
    });

    REQUIRE(result.has_value());
    const auto sends = fix.messenger.sends();
    REQUIRE(sends.size() == 1);
    CHECK_THAT(sends.front().html, ContainsSubstring("movie.mkv"));
}

TEST_CASE("ShowStatus hides other users' tasks unless requested", "[application][show_status]") {
    Fixture fix;
    auto own_task = make_task("task-own");
    own_task.attach_downloader(Gid{std::string{"own-gid"}}, DownloaderKind::Aria2);
    auto other_task = make_task("task-other", UserId{99});
    other_task.attach_downloader(Gid{std::string{"other-gid"}}, DownloaderKind::Aria2);
    fix.aria2.push_status(status_snapshot("own-gid", "own.bin"));
    fix.aria2.push_status(status_snapshot("other-gid", "other.bin"));

    asio::io_context ctx;
    auto saved = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        auto first = co_await fix.tasks.save(own_task);
        if (!first) {
            co_return std::unexpected(first.error());
        }
        co_return co_await fix.tasks.save(other_task);
    });
    REQUIRE(saved.has_value());

    auto use_case = fix.make();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await use_case.execute(StatusRequest{
            .user = UserId{42},
            .chat = ChatId{-100123},
        });
    });

    REQUIRE(result.has_value());
    const auto sends = fix.messenger.sends();
    REQUIRE(sends.size() == 1);
    CHECK_THAT(sends.front().html, ContainsSubstring("own.bin"));
    CHECK_THAT(sends.front().html, !ContainsSubstring("other.bin"));
}

TEST_CASE("ShowStatus can include all users for admins", "[application][show_status]") {
    Fixture fix;
    auto own_task = make_task("a-own");
    own_task.attach_downloader(Gid{std::string{"own-gid"}}, DownloaderKind::Aria2);
    auto other_task = make_task("b-other", UserId{99});
    other_task.attach_downloader(Gid{std::string{"other-gid"}}, DownloaderKind::Aria2);
    fix.aria2.push_status(status_snapshot("own-gid", "own.bin"));
    fix.aria2.push_status(status_snapshot("other-gid", "other.bin"));

    asio::io_context ctx;
    auto saved = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        auto first = co_await fix.tasks.save(own_task);
        if (!first) {
            co_return std::unexpected(first.error());
        }
        co_return co_await fix.tasks.save(other_task);
    });
    REQUIRE(saved.has_value());

    auto use_case = fix.make();
    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await use_case.execute(StatusRequest{
            .user = UserId{42},
            .chat = ChatId{-100123},
            .include_all_users = true,
        });
    });

    REQUIRE(result.has_value());
    const auto sends = fix.messenger.sends();
    REQUIRE(sends.size() == 1);
    CHECK_THAT(sends.front().html, ContainsSubstring("own.bin"));
    CHECK_THAT(sends.front().html, ContainsSubstring("other.bin"));
}
