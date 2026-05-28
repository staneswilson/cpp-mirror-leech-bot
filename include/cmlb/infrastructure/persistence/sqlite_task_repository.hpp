#pragma once

#include <optional>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>

namespace cmlb::infrastructure::persistence {

/// SQLite-backed implementation of `TaskRepository`. Stateless beyond a
/// reference to the shared connection pool — instances are cheap to construct
/// and safe to share across coroutines.
class SqliteTaskRepository final : public TaskRepository {
public:
    explicit SqliteTaskRepository(SqliteConnectionPool& pool) noexcept : pool_{pool} {}

    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
    save(domain::Task task) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<std::optional<domain::Task>>>
    find(domain::TaskId id) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<std::vector<domain::Task>>>
    incomplete() override;

    [[nodiscard]] boost::asio::awaitable<core::Result<std::vector<domain::Task>>>
    for_user(domain::UserId user) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
    remove(domain::TaskId id) override;

private:
    SqliteConnectionPool& pool_;
};

}  // namespace cmlb::infrastructure::persistence
