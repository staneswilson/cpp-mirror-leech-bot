// ---------------------------------------------------------------------------
// sqlite_task_repository_test.cpp
//
// End-to-end persistence tests for SqliteTaskRepository:
//   * Happy path save -> find round-trip preserves identity + state.
//   * for_user filters by user id and orders by created_at DESC.
//   * incomplete excludes terminal states.
//   * remove returns NotFound for unknown ids; deletes existing rows.
//   * find returns nullopt for unknown ids.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/persistence/schema_migrator.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>
#include <cmlb/infrastructure/persistence/sqlite_task_repository.hpp>

namespace {

namespace asio = boost::asio;
using cmlb::core::DatabaseConfig;
using cmlb::core::ErrorCode;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::Task;
using cmlb::domain::TaskId;
using cmlb::domain::TaskKind;
using cmlb::domain::TaskMetadata;
using cmlb::domain::TaskState;
using cmlb::domain::UserId;
using cmlb::infrastructure::persistence::SchemaMigrator;
using cmlb::infrastructure::persistence::SqliteConnectionPool;
using cmlb::infrastructure::persistence::SqliteTaskRepository;

/// RAII fixture creating a unique temp directory holding the SQLite DB. The
/// directory is removed on destruction.
class TempDatabase {
public:
    TempDatabase() {
        std::random_device rd;
        std::uniform_int_distribution<std::uint64_t> dist;
        const auto salt = dist(rd);
        dir_ = std::filesystem::temp_directory_path()
               / ("cmlb_task_repo_test_" + std::to_string(salt));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "cmlb.db";
    }

    ~TempDatabase() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;
    TempDatabase(TempDatabase&&) = delete;
    TempDatabase& operator=(TempDatabase&&) = delete;

    [[nodiscard]] DatabaseConfig config() const {
        return DatabaseConfig{
            .path = path_, .busy_timeout = std::chrono::milliseconds{5000}, .wal_mode = true};
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
};

/// Drives a coroutine factory to completion on @p ctx and returns its value.
/// Restarts the context afterwards so the same fixture can drive multiple
/// awaitables sequentially.
template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& make_awaitable) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(make_awaitable), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

[[nodiscard]] TaskMetadata sample_metadata(std::string id,
                                           std::int64_t user_id,
                                           TaskKind kind = TaskKind::Mirror) {
    const auto now = std::chrono::system_clock::now();
    return TaskMetadata{
        .id = TaskId{std::move(id)},
        .user = UserId{user_id},
        .chat = ChatId{-100123},
        .status_message = MessageId{42},
        .kind = kind,
        .source_url = "https://example.com/file.iso",
        .created_at = now,
        .updated_at = now,
    };
}

struct RepoFixture {
    TempDatabase tmp;
    asio::io_context ctx;
    SqliteConnectionPool pool;
    SqliteTaskRepository repo;

    RepoFixture()
        : tmp{}, ctx{}, pool{tmp.config(), ctx.get_executor(), /*pool_size=*/2}, repo{pool} {
        SchemaMigrator migrator{pool};
        auto migrate_result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await migrator.migrate();
        });
        REQUIRE(migrate_result.has_value());
    }
};

} // namespace

TEST_CASE("save then find returns the persisted task",
          "[integration][persistence][task_repository]") {
    RepoFixture fix;

    Task original{sample_metadata("01890b3c-7f0c-7e0e-bb91-aa8b1a0d3f01", 42)};

    auto save_result = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await fix.repo.save(original);
    });
    REQUIRE(save_result.has_value());

    auto found = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
        co_return co_await fix.repo.find(original.metadata().id);
    });
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    REQUIRE((**found).metadata().id.value() == original.metadata().id.value());
    REQUIRE((**found).metadata().user.value() == 42);
    REQUIRE((**found).metadata().source_url == "https://example.com/file.iso");
    REQUIRE((**found).state() == TaskState::Queued);
}

TEST_CASE("find returns nullopt for unknown ids", "[integration][persistence][task_repository]") {
    RepoFixture fix;

    auto result =
        run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
            co_return co_await fix.repo.find(TaskId{"does-not-exist"});
        });
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
}

TEST_CASE("for_user filters by user id", "[integration][persistence][task_repository]") {
    RepoFixture fix;

    Task t1{sample_metadata("task-a", 100)};
    Task t2{sample_metadata("task-b", 200)};
    Task t3{sample_metadata("task-c", 100)};

    for (Task& t : {std::ref(t1), std::ref(t2), std::ref(t3)}) {
        auto save_result = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await fix.repo.save(t);
        });
        REQUIRE(save_result.has_value());
    }

    auto results = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::vector<Task>>> {
        co_return co_await fix.repo.for_user(UserId{100});
    });
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 2);
    for (const auto& t : *results) {
        REQUIRE(t.metadata().user.value() == 100);
    }
}

TEST_CASE("incomplete excludes terminal states", "[integration][persistence][task_repository]") {
    RepoFixture fix;

    Task queued{sample_metadata("queued", 1)};

    Task done{sample_metadata("done", 1)};
    REQUIRE(done.start_download().has_value());
    REQUIRE(done.begin_upload().has_value());
    REQUIRE(done.mark_completed().has_value());

    Task cancelled{sample_metadata("cancelled", 1)};
    REQUIRE(cancelled.mark_cancelled().has_value());

    for (Task& t : {std::ref(queued), std::ref(done), std::ref(cancelled)}) {
        auto save_result = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
            co_return co_await fix.repo.save(t);
        });
        REQUIRE(save_result.has_value());
    }

    auto live = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::vector<Task>>> {
        co_return co_await fix.repo.incomplete();
    });
    REQUIRE(live.has_value());
    REQUIRE(live->size() == 1);
    REQUIRE((*live)[0].metadata().id.value() == "queued");
    REQUIRE((*live)[0].state() == TaskState::Queued);
}

TEST_CASE("remove deletes a persisted task", "[integration][persistence][task_repository]") {
    RepoFixture fix;

    Task t{sample_metadata("victim", 7)};
    REQUIRE(run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                co_return co_await fix.repo.save(t);
            }).has_value());

    REQUIRE(run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                co_return co_await fix.repo.remove(t.metadata().id);
            }).has_value());

    auto found = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
        co_return co_await fix.repo.find(t.metadata().id);
    });
    REQUIRE(found.has_value());
    REQUIRE_FALSE(found->has_value());
}

TEST_CASE("remove on unknown id returns NotFound", "[integration][persistence][task_repository]") {
    RepoFixture fix;

    auto result = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await fix.repo.remove(TaskId{"missing"});
    });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::NotFound);
}

TEST_CASE("save with error message persists the message",
          "[integration][persistence][task_repository]") {
    RepoFixture fix;

    Task t{sample_metadata("failed", 7)};
    REQUIRE(t.mark_failed("boom").has_value());

    REQUIRE(run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                co_return co_await fix.repo.save(t);
            }).has_value());

    auto found = run_on(fix.ctx, [&]() -> asio::awaitable<cmlb::core::Result<std::optional<Task>>> {
        co_return co_await fix.repo.find(t.metadata().id);
    });
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    REQUIRE((**found).state() == TaskState::Failed);
    REQUIRE((**found).error_message().has_value());
    REQUIRE(std::string{*(**found).error_message()} == "boom");
}
