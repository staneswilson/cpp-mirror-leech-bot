#pragma once

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>

namespace cmlb::test_support {

/// In-memory TaskRepository for unit tests. Behavior mirrors the SQLite
/// implementation as closely as needed for application-layer test coverage.
class InMemoryTaskRepository final
    : public cmlb::infrastructure::persistence::TaskRepository {
public:
    InMemoryTaskRepository()  = default;
    ~InMemoryTaskRepository() override = default;

    boost::asio::awaitable<cmlb::core::Result<void>>
    save(cmlb::domain::Task task) override {
        std::lock_guard lk{mutex_};
        const auto id = task.metadata().id;
        // map keeps a single copy: insert or replace.
        store_.insert_or_assign(id, std::move(task));
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<std::optional<cmlb::domain::Task>>>
    find(cmlb::domain::TaskId id) override {
        std::lock_guard lk{mutex_};
        auto it = store_.find(id);
        if (it == store_.end()) {
            co_return std::optional<cmlb::domain::Task>{};
        }
        co_return std::optional<cmlb::domain::Task>{it->second};
    }

    boost::asio::awaitable<cmlb::core::Result<std::vector<cmlb::domain::Task>>>
    incomplete() override {
        std::lock_guard lk{mutex_};
        std::vector<cmlb::domain::Task> out;
        for (const auto& [_, task] : store_) {
            if (!task.is_terminal()) out.push_back(task);
        }
        co_return out;
    }

    boost::asio::awaitable<cmlb::core::Result<std::vector<cmlb::domain::Task>>>
    for_user(cmlb::domain::UserId user) override {
        std::lock_guard lk{mutex_};
        std::vector<cmlb::domain::Task> out;
        for (const auto& [_, task] : store_) {
            if (task.metadata().user == user) out.push_back(task);
        }
        std::ranges::sort(out, [](const auto& a, const auto& b) {
            return a.metadata().created_at > b.metadata().created_at;
        });
        co_return out;
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    remove(cmlb::domain::TaskId id) override {
        std::lock_guard lk{mutex_};
        const auto erased = store_.erase(id);
        if (erased == 0) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                        "task not found");
        }
        co_return cmlb::core::Result<void>{};
    }

    /// Test helper: total number of stored tasks (any state).
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lk{mutex_};
        return store_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<cmlb::domain::TaskId, cmlb::domain::Task> store_;
};

}  // namespace cmlb::test_support
