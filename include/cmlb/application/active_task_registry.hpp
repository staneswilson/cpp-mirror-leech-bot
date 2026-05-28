#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <cmlb/domain/identifiers.hpp>

/// @file active_task_registry.hpp
/// @brief Cross-use-case cancellation channel keyed by `TaskId`.
///
/// Running use cases (`MirrorUrl`, `LeechUrl`, ...) register an
/// `std::atomic<bool>` flag on entry and consult it in their poll loop. The
/// `CancelTask` use case looks up the flag by `TaskId` and sets it, which
/// allows the running coroutine to observe the request and unwind its own
/// resources cleanly (in-flight uploads, downloader state, status message).
/// Without this channel, `/cancel` would only update the DB row and call
/// `downloader.remove`, leaving the coroutine to keep pumping bytes until it
/// noticed the backend removal — slow on big uploads.

namespace cmlb::application {

/// Thread-safe registry of in-flight tasks and their cancellation flags.
class ActiveTaskRegistry {
public:
    ActiveTaskRegistry() = default;
    ActiveTaskRegistry(const ActiveTaskRegistry&)            = delete;
    ActiveTaskRegistry& operator=(const ActiveTaskRegistry&) = delete;
    ActiveTaskRegistry(ActiveTaskRegistry&&)                 = delete;
    ActiveTaskRegistry& operator=(ActiveTaskRegistry&&)      = delete;
    ~ActiveTaskRegistry()                                    = default;

    /// Registers @p id and returns the cancellation flag. Caller polls the
    /// flag; `CancelTask` sets it. Re-registering an id keeps the same flag.
    [[nodiscard]] std::shared_ptr<std::atomic<bool>>
    register_task(cmlb::domain::TaskId id) {
        std::lock_guard lk{mu_};
        auto& slot = map_[id];
        if (!slot) {
            slot = std::make_shared<std::atomic<bool>>(false);
        }
        return slot;
    }

    /// Removes @p id. No-op if absent. Existing `shared_ptr`s held by the
    /// use case stay valid until they go out of scope.
    void unregister(cmlb::domain::TaskId id) noexcept {
        std::lock_guard lk{mu_};
        map_.erase(id);
    }

    /// Sets the cancellation flag for @p id. Returns `true` if the task was
    /// registered (i.e. the running coroutine will observe the flag), and
    /// `false` if no entry exists — typically because the use case has
    /// already exited or the task only lives in the DB.
    bool cancel(cmlb::domain::TaskId id) noexcept {
        std::shared_ptr<std::atomic<bool>> flag;
        {
            std::lock_guard lk{mu_};
            auto it = map_.find(id);
            if (it == map_.end()) return false;
            flag = it->second;
        }
        if (flag) {
            flag->store(true, std::memory_order_release);
        }
        return true;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<cmlb::domain::TaskId,
                       std::shared_ptr<std::atomic<bool>>>
        map_;
};

/// RAII helper that registers a task on construction and unregisters it on
/// destruction. Use cases create one near the top of `execute` so every exit
/// path (normal, cancel, failure, exception) deregisters automatically.
class ActiveTaskGuard {
public:
    ActiveTaskGuard(ActiveTaskRegistry& registry, cmlb::domain::TaskId id)
        : registry_{&registry},
          id_{id},
          flag_{registry.register_task(id)} {}

    ActiveTaskGuard(const ActiveTaskGuard&)            = delete;
    ActiveTaskGuard& operator=(const ActiveTaskGuard&) = delete;
    ActiveTaskGuard(ActiveTaskGuard&&)                 = delete;
    ActiveTaskGuard& operator=(ActiveTaskGuard&&)      = delete;

    ~ActiveTaskGuard() noexcept {
        if (registry_) registry_->unregister(id_);
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::shared_ptr<std::atomic<bool>>& flag()
        const noexcept {
        return flag_;
    }

private:
    ActiveTaskRegistry*               registry_;
    cmlb::domain::TaskId              id_;
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace cmlb::application
