#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

namespace cmlb::core {

/// Owns a `boost::asio::io_context` and a pool of worker threads driving it.
///
/// Used as the universal execution substrate: subsystems (Telegram gateway,
/// downloaders, uploaders) spawn awaitables onto `get_executor()` or post
/// fire-and-forget callbacks via `post()`. A `work_guard` keeps the context
/// alive until `stop()` is invoked.
class Executor {
public:
    /// Starts `worker_count` worker threads that run the io_context. Falls
    /// back to 2 workers if `std::thread::hardware_concurrency()` reports 0.
    explicit Executor(std::size_t worker_count = std::thread::hardware_concurrency());

    /// Stops the io_context and joins workers.
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    /// Releases the work guard, stops the io_context, and joins workers.
    /// Idempotent — safe to call multiple times.
    void stop() noexcept;

    /// Returns the underlying io_context (for APIs that demand the raw type).
    [[nodiscard]] boost::asio::io_context& io_context() noexcept {
        return io_context_;
    }

    /// Returns a type-erased executor suitable for `co_spawn` etc.
    [[nodiscard]] boost::asio::any_io_executor get_executor() noexcept {
        return io_context_.get_executor();
    }

    /// Returns a fresh strand bound to this executor — used for serialized
    /// regions (e.g. all TDLib API calls).
    [[nodiscard]] boost::asio::strand<boost::asio::any_io_executor> make_strand() {
        return boost::asio::make_strand(get_executor());
    }

    /// Fire-and-forget post of a nullary callable onto the io_context.
    template <typename F>
    void post(F&& f) {
        boost::asio::post(io_context_, std::forward<F>(f));
    }

    /// Number of worker threads.
    [[nodiscard]] std::size_t worker_count() const noexcept {
        return workers_.size();
    }

private:
    boost::asio::io_context io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::vector<std::thread> workers_;
    std::mutex stop_mutex_;
    std::atomic<bool> stopped_{false};
};

} // namespace cmlb::core
