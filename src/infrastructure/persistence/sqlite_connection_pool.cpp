#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <fmt/format.h>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

namespace cmlb::infrastructure::persistence {

namespace asio = boost::asio;

namespace {

[[nodiscard]] std::shared_ptr<sqlite::database> open_one(const core::DatabaseConfig& cfg) {
    // Ensure the directory exists; sqlite-modern-cpp will not create it.
    if (cfg.path.has_parent_path()) {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(cfg.path.parent_path(), mkdir_ec);
        if (mkdir_ec) {
            throw std::system_error{mkdir_ec,
                                    "Failed to create SQLite parent directory: "
                                        + cfg.path.parent_path().string()};
        }
    }

    auto db = std::make_shared<sqlite::database>(cfg.path.string());

    if (cfg.wal_mode) {
        *db << "PRAGMA journal_mode = WAL;";
    }
    *db << "PRAGMA synchronous = NORMAL;";
    *db << fmt::format("PRAGMA busy_timeout = {};", cfg.busy_timeout.count());
    *db << "PRAGMA foreign_keys = ON;";
    *db << "PRAGMA temp_store = MEMORY;";

    // ---- Throughput tuning ------------------------------------------------
    // mmap_size: maps up to 256 MiB of the DB file into address space so reads
    // bypass the OS read() syscall and avoid copying through the page cache.
    *db << "PRAGMA mmap_size = 268435456;";
    // cache_size: negative value = KiB. 65536 KiB = 64 MiB per connection. The
    // working set of bot_settings/user_settings/tasks easily fits in this.
    *db << "PRAGMA cache_size = -65536;";
    // wal_autocheckpoint: trigger a WAL → main checkpoint every 1000 pages
    // (~4 MiB). Bounds WAL growth without blocking writers for too long.
    *db << "PRAGMA wal_autocheckpoint = 1000;";
    // Hint the optimizer once at open time. Cheap and idempotent.
    *db << "PRAGMA optimize;";

    return db;
}

} // namespace

// ---------------------------------------------------------------------------
// SqliteConnectionHandle
// ---------------------------------------------------------------------------

void SqliteConnectionHandle::release_() noexcept {
    if (pool_ != nullptr && slot_ != static_cast<std::size_t>(-1)) {
        pool_->return_slot_(slot_);
    }
    pool_ = nullptr;
    slot_ = static_cast<std::size_t>(-1);
    db_.reset();
}

// ---------------------------------------------------------------------------
// SqliteConnectionPool
// ---------------------------------------------------------------------------

SqliteConnectionPool::SqliteConnectionPool(core::DatabaseConfig config,
                                           asio::any_io_executor executor,
                                           std::size_t pool_size)
    : config_{std::move(config)}, executor_{std::move(executor)} {
    if (pool_size == 0) {
        throw std::invalid_argument{"SqliteConnectionPool: pool_size must be >= 1"};
    }

    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.push_back(open_one(config_));
    }

    // Channel capacity == pool size: every slot index sits in the channel
    // initially and is consumed by acquire().
    free_slots_ = std::make_unique<Channel>(executor_, pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        boost::system::error_code ec;
        free_slots_->try_send(ec, i);
        if (ec) {
            // Should not happen: capacity matches insertions.
            throw std::runtime_error{"SqliteConnectionPool: failed to prime free-slot channel: "
                                     + ec.message()};
        }
    }
}

SqliteConnectionPool::~SqliteConnectionPool() {
    if (free_slots_) {
        free_slots_->close();
    }
}

asio::awaitable<core::Result<SqliteConnectionHandle>> SqliteConnectionPool::acquire() {
    boost::system::error_code ec;
    const auto slot =
        co_await free_slots_->async_receive(asio::redirect_error(asio::use_awaitable, ec));

    if (ec) {
        co_return core::error(core::ErrorCode::ResourceExhausted,
                              "SqliteConnectionPool::acquire failed: " + ec.message());
    }

    std::shared_ptr<sqlite::database> conn;
    {
        const std::lock_guard guard{mutex_};
        conn = connections_.at(slot);
    }
    co_return SqliteConnectionHandle{this, slot, std::move(conn)};
}

void SqliteConnectionPool::return_slot_(std::size_t slot) noexcept {
    if (!free_slots_) {
        return;
    }
    boost::system::error_code ec;
    free_slots_->try_send(ec, slot);
    // try_send only fails if the channel is full or closed; in either case
    // we have nothing useful to do at handle-destruction time.
    (void)ec;
}

asio::awaitable<core::Result<void>> SqliteConnectionPool::ping() {
    auto acquired = co_await acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }

    try {
        int probe = 0;
        acquired->database() << "SELECT 1;" >> probe;
        if (probe != 1) {
            co_return core::error(core::ErrorCode::Database,
                                  "ping: SELECT 1 returned " + std::to_string(probe));
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database, std::string{"ping failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database, std::string{"ping threw: "} + ex.what());
    }
}

} // namespace cmlb::infrastructure::persistence
