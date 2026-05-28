#pragma once

#include <optional>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>

/// @file task_repository.hpp
/// @brief Abstract repository over the `Task` aggregate.
///
/// Implementations persist the task's state machine snapshot — they do not
/// (and should not) replay the transitions. Reconstitution restores the full
/// metadata + current state + last error message in a single read.

namespace cmlb::infrastructure::persistence {

class TaskRepository {
public:
    virtual ~TaskRepository() = default;

    TaskRepository() = default;
    TaskRepository(const TaskRepository&) = delete;
    TaskRepository& operator=(const TaskRepository&) = delete;
    TaskRepository(TaskRepository&&) = delete;
    TaskRepository& operator=(TaskRepository&&) = delete;

    /// Insert-or-replace by primary key (`TaskMetadata::id`). Idempotent.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> save(domain::Task task) = 0;

    /// Returns the persisted task, or `std::nullopt` if no row matches `id`.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::optional<domain::Task>>> find(
        domain::TaskId id) = 0;

    /// Returns every task whose state is not Completed, Failed, or Cancelled.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::vector<domain::Task>>>
    incomplete() = 0;

    /// Returns every task owned by the given user, regardless of state, in
    /// `created_at DESC` order.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::vector<domain::Task>>> for_user(
        domain::UserId user) = 0;

    /// Deletes the row identified by `id`. Returns `NotFound` if no row matched.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> remove(domain::TaskId id) = 0;
};

} // namespace cmlb::infrastructure::persistence
