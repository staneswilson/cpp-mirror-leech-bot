#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>

/// @file sqlite_connection_pool.hpp
/// @brief Async-friendly pool of `sqlite::database` connections.
///
/// The pool owns a small fixed number of SQLite connections, each pre-configured
/// with the canonical CMLB pragmas (WAL journal, NORMAL sync, busy timeout,
/// foreign keys, in-memory temp store). Consumers acquire a connection via an
/// `awaitable<...>` that yields to the executor until a slot frees up; the
/// returned RAII handle returns the connection to the pool on destruction.

namespace cmlb::infrastructure::persistence {

class SqliteConnectionPool;

/// RAII handle for a borrowed connection. Returns the connection to the
/// owning pool on destruction. Move-only.
class SqliteConnectionHandle {
public:
    SqliteConnectionHandle() = default;

    SqliteConnectionHandle(const SqliteConnectionHandle&) = delete;
    SqliteConnectionHandle& operator=(const SqliteConnectionHandle&) = delete;

    SqliteConnectionHandle(SqliteConnectionHandle&& other) noexcept
        : pool_{other.pool_}, slot_{other.slot_}, db_{std::move(other.db_)} {
        other.pool_ = nullptr;
        other.slot_ = static_cast<std::size_t>(-1);
    }

    SqliteConnectionHandle& operator=(SqliteConnectionHandle&& other) noexcept {
        if (this != &other) {
            release_();
            pool_ = other.pool_;
            slot_ = other.slot_;
            db_ = std::move(other.db_);
            other.pool_ = nullptr;
            other.slot_ = static_cast<std::size_t>(-1);
        }
        return *this;
    }

    ~SqliteConnectionHandle() {
        release_();
    }

    /// Returns the raw `sqlite::database` ref. Lifetime is tied to *this*.
    [[nodiscard]] sqlite::database& database() noexcept {
        return *db_;
    }

    [[nodiscard]] const sqlite::database& database() const noexcept {
        return *db_;
    }

    /// True iff this handle currently owns a borrowed connection.
    [[nodiscard]] bool valid() const noexcept {
        return pool_ != nullptr && db_ != nullptr;
    }

private:
    friend class SqliteConnectionPool;

    SqliteConnectionHandle(SqliteConnectionPool* pool,
                           std::size_t slot,
                           std::shared_ptr<sqlite::database> db) noexcept
        : pool_{pool}, slot_{slot}, db_{std::move(db)} {
    }

    void release_() noexcept;

    SqliteConnectionPool* pool_{nullptr};
    std::size_t slot_{static_cast<std::size_t>(-1)};
    std::shared_ptr<sqlite::database> db_;
};

/// Fixed-capacity pool of SQLite connections.
///
/// All connections target the same database file declared in
/// `core::DatabaseConfig::path` and share identical pragma settings. Acquisition
/// is asynchronous and back-pressured by an `experimental::channel` of
/// available slots — when every connection is borrowed, callers suspend until
/// one is returned.
class SqliteConnectionPool {
public:
    /// Default pool capacity (number of distinct connections opened on
    /// construction). Sized for parallel readers under WAL mode — SQLite still
    /// serialises writers, but a larger pool keeps repository reads from
    /// blocking each other while a single writer holds the lock.
    static constexpr std::size_t kDefaultPoolSize = 8;

    /// Constructs and opens the pool. Throws `std::system_error` (wrapping the
    /// SQLite error message) if any connection fails to open or its pragmas
    /// fail to apply. The synchronous failure mode is deliberate — a process
    /// that cannot open its database has no useful runtime to recover into.
    SqliteConnectionPool(core::DatabaseConfig config,
                         boost::asio::any_io_executor executor,
                         std::size_t pool_size = kDefaultPoolSize);

    ~SqliteConnectionPool();

    SqliteConnectionPool(const SqliteConnectionPool&) = delete;
    SqliteConnectionPool& operator=(const SqliteConnectionPool&) = delete;
    SqliteConnectionPool(SqliteConnectionPool&&) = delete;
    SqliteConnectionPool& operator=(SqliteConnectionPool&&) = delete;

    /// Borrow a connection. The returned handle releases the connection on
    /// destruction. Cancellation of the awaitable leaves pool state intact.
    [[nodiscard]] boost::asio::awaitable<core::Result<SqliteConnectionHandle>> acquire();

    /// `SELECT 1` round-trip used by health checks.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> ping();

    /// Number of connections in the pool (fixed for the lifetime of *this*).
    [[nodiscard]] std::size_t size() const noexcept {
        return connections_.size();
    }

    /// Returns the executor passed to the constructor.
    [[nodiscard]] boost::asio::any_io_executor get_executor() const noexcept {
        return executor_;
    }

    /// Returns the on-disk path the pool is bound to.
    [[nodiscard]] const core::DatabaseConfig& config() const noexcept {
        return config_;
    }

private:
    friend class SqliteConnectionHandle;

    using Channel =
        boost::asio::experimental::channel<void(boost::system::error_code, std::size_t)>;

    void return_slot_(std::size_t slot) noexcept;

    core::DatabaseConfig config_;
    boost::asio::any_io_executor executor_;
    std::vector<std::shared_ptr<sqlite::database>> connections_;
    std::unique_ptr<Channel> free_slots_;
    std::mutex mutex_;
};

} // namespace cmlb::infrastructure::persistence
