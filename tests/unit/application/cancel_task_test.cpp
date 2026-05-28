// ---------------------------------------------------------------------------
// cancel_task_test.cpp - unit tests for CancelTask use case.
// ---------------------------------------------------------------------------

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/cancel_task.hpp>
#include <cmlb/domain/task.hpp>

#include "in_memory_task_repository.hpp"
#include "stub_downloader.hpp"
#include "stub_messenger.hpp"

namespace asio = boost::asio;

using cmlb::application::ActiveTaskRegistry;
using cmlb::application::CancelTask;
using cmlb::application::CancelTaskRequest;
using cmlb::core::ErrorCode;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::Task;
using cmlb::domain::TaskId;
using cmlb::domain::TaskKind;
using cmlb::domain::TaskMetadata;
using cmlb::domain::UserId;
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

TaskMetadata sample_metadata(std::string id = "tid-1") {
    const auto now = std::chrono::system_clock::now();
    return TaskMetadata{
        .id             = TaskId{std::move(id)},
        .user           = UserId{42},
        .chat           = ChatId{-100123},
        .status_message = MessageId{7},
        .kind           = TaskKind::Mirror,
        .source_url     = "https://example.com/file.iso",
        .created_at     = now,
        .updated_at     = now,
    };
}

}  // namespace

TEST_CASE("CancelTask returns NotFound when the task does not exist",
          "[application][cancel_task]") {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    StubMessenger messenger;
    ActiveTaskRegistry active_tasks;
    CancelTask uc{tasks, aria2, qbit, messenger, active_tasks};

    asio::io_context ctx;
    auto result = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.execute(
            CancelTaskRequest{TaskId{"missing"}, ChatId{-1}});
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::NotFound);
}

TEST_CASE("CancelTask rejects already-terminal tasks",
          "[application][cancel_task]") {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    StubMessenger messenger;
    ActiveTaskRegistry active_tasks;

    Task task{sample_metadata("done")};
    REQUIRE(task.start_download().has_value());
    REQUIRE(task.begin_upload().has_value());
    REQUIRE(task.mark_completed().has_value());

    asio::io_context ctx;
    (void)run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await tasks.save(task);
    });

    CancelTask uc{tasks, aria2, qbit, messenger, active_tasks};
    auto result = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.execute(
            CancelTaskRequest{task.metadata().id, ChatId{-100123}});
    });

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidState);
}

TEST_CASE("CancelTask happy path: marks task cancelled and notifies user",
          "[application][cancel_task]") {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    StubMessenger messenger;
    ActiveTaskRegistry active_tasks;

    Task task{sample_metadata("active")};
    REQUIRE(task.start_download().has_value());
    // CancelTask dispatches by `task.downloader_kind()`; bind aria2 so we
    // can assert exactly one downloader receives the remove() call.
    task.attach_downloader(cmlb::domain::Gid{std::string{"abc-gid"}},
                           cmlb::domain::DownloaderKind::Aria2);

    asio::io_context ctx;
    (void)run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await tasks.save(task);
    });

    CancelTask uc{tasks, aria2, qbit, messenger, active_tasks};
    auto result = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.execute(
            CancelTaskRequest{task.metadata().id, ChatId{-100123}});
    });

    REQUIRE(result.has_value());
    // Only the bound backend (aria2) is asked to remove; qbit stays idle.
    CHECK(aria2.remove_calls() == 1);
    CHECK(qbit.remove_calls() == 0);
    CHECK_FALSE(messenger.sends().empty());

    // Persisted task is now Cancelled.
    auto loaded = run_on(ctx, [&]()
        -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
        co_return co_await tasks.find(task.metadata().id);
    });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((**loaded).state() == cmlb::domain::TaskState::Cancelled);
}

TEST_CASE("CancelTask delegates teardown when a coroutine is registered",
          "[application][cancel_task]") {
    InMemoryTaskRepository tasks;
    StubDownloader aria2;
    StubDownloader qbit;
    StubMessenger messenger;
    ActiveTaskRegistry active_tasks;

    Task task{sample_metadata("live")};
    REQUIRE(task.start_download().has_value());
    task.attach_downloader(cmlb::domain::Gid{std::string{"live-gid"}},
                           cmlb::domain::DownloaderKind::Aria2);

    asio::io_context ctx;
    (void)run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await tasks.save(task);
    });

    // Simulate an in-flight use-case coroutine by registering its task id.
    auto cancel_flag = active_tasks.register_task(task.metadata().id);

    CancelTask uc{tasks, aria2, qbit, messenger, active_tasks};
    auto result = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.execute(
            CancelTaskRequest{task.metadata().id, ChatId{-100123}});
    });

    REQUIRE(result.has_value());
    // The flag is set so the live coroutine will observe + tear down. We must
    // NOT do the backend remove or DB write here.
    CHECK(cancel_flag->load(std::memory_order_acquire) == true);
    CHECK(aria2.remove_calls() == 0);
    CHECK(qbit.remove_calls() == 0);
    // A short "Cancelling" ack is sent so the user gets immediate feedback.
    CHECK_FALSE(messenger.sends().empty());

    // The DB row is still in Downloading — the live coroutine is responsible
    // for transitioning it to Cancelled once it observes the flag.
    auto loaded = run_on(ctx, [&]()
        -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
        co_return co_await tasks.find(task.metadata().id);
    });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((**loaded).state() == cmlb::domain::TaskState::Downloading);
}
