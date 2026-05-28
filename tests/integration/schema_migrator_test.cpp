// ---------------------------------------------------------------------------
// schema_migrator_test.cpp
//
// Exercises SchemaMigrator against a fresh on-disk SQLite database:
//   * Migrating from an empty database advances to the registry's max version.
//   * Re-running migrate() on an already-migrated database is a no-op.
//   * The tables declared by each migration exist after running.
//   * current_version on a virgin DB reports 0.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <sqlite_modern_cpp.h>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/schema_migrator.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

namespace {

namespace asio = boost::asio;
using cmlb::core::DatabaseConfig;
using cmlb::infrastructure::persistence::SchemaMigrator;
using cmlb::infrastructure::persistence::SqliteConnectionPool;

class TempDatabase {
public:
    TempDatabase() {
        std::random_device rd;
        std::uniform_int_distribution<std::uint64_t> dist;
        const auto salt = dist(rd);
        dir_ =
            std::filesystem::temp_directory_path() / ("cmlb_migrator_test_" + std::to_string(salt));
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

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] DatabaseConfig config() const {
        return DatabaseConfig{
            .path = path_, .busy_timeout = std::chrono::milliseconds{5000}, .wal_mode = true};
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
};

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& factory) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(factory), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

[[nodiscard]] bool table_exists(const std::filesystem::path& db_path, const std::string& name) {
    sqlite::database db{db_path.string()};
    int count = 0;
    db << "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name = ?;" << name >> count;
    return count == 1;
}

} // namespace

TEST_CASE("Fresh database migrates to the highest registered version",
          "[integration][persistence][migrator]") {
    TempDatabase tmp;
    asio::io_context ctx;
    SqliteConnectionPool pool{tmp.config(), ctx.get_executor(), /*pool_size=*/2};
    SchemaMigrator migrator{pool};

    auto migrate_result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await migrator.migrate();
    });
    REQUIRE(migrate_result.has_value());

    auto version_result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<int>> {
        co_return co_await migrator.current_version();
    });
    REQUIRE(version_result.has_value());

    const int expected = SchemaMigrator::registry().back().version;
    REQUIRE(*version_result == expected);
}

TEST_CASE("Double-applying migrate() is idempotent", "[integration][persistence][migrator]") {
    TempDatabase tmp;
    asio::io_context ctx;
    SqliteConnectionPool pool{tmp.config(), ctx.get_executor(), /*pool_size=*/2};
    SchemaMigrator migrator{pool};

    REQUIRE(run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                co_return co_await migrator.migrate();
            }).has_value());

    const int first = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<int>> {
                          co_return co_await migrator.current_version();
                      }).value();

    REQUIRE(run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                co_return co_await migrator.migrate();
            }).has_value());

    const int second = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<int>> {
                           co_return co_await migrator.current_version();
                       }).value();

    REQUIRE(first == second);
}

TEST_CASE("After migration the canonical tables exist", "[integration][persistence][migrator]") {
    TempDatabase tmp;
    {
        asio::io_context ctx;
        SqliteConnectionPool pool{tmp.config(), ctx.get_executor(), /*pool_size=*/1};
        SchemaMigrator migrator{pool};
        REQUIRE(run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
                    co_return co_await migrator.migrate();
                }).has_value());
    } // pool dtor closes connections before sqlite::database{} re-opens below.

    REQUIRE(table_exists(tmp.path(), "schema_version"));
    REQUIRE(table_exists(tmp.path(), "bot_settings"));
    REQUIRE(table_exists(tmp.path(), "user_settings"));
    REQUIRE(table_exists(tmp.path(), "tasks"));
    REQUIRE(table_exists(tmp.path(), "rss_feeds"));
}

TEST_CASE("current_version on a fresh DB returns 0 before migrate",
          "[integration][persistence][migrator]") {
    TempDatabase tmp;
    asio::io_context ctx;
    SqliteConnectionPool pool{tmp.config(), ctx.get_executor(), /*pool_size=*/1};
    SchemaMigrator migrator{pool};

    auto v = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<int>> {
        co_return co_await migrator.current_version();
    });
    REQUIRE(v.has_value());
    REQUIRE(*v == 0);
}
