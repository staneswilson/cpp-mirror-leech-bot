#include <cstddef>
#include <mutex>
#include <thread>

#include <boost/asio/io_context.hpp>

#include <cmlb/core/executor.hpp>

namespace cmlb::core {

namespace {

std::size_t resolve_worker_count(std::size_t requested) noexcept {
    if (requested != 0)
        return requested;
    const auto hw = std::thread::hardware_concurrency();
    return hw == 0 ? std::size_t{2} : static_cast<std::size_t>(hw);
}

} // namespace

Executor::Executor(std::size_t worker_count)
    : io_context_{}, work_guard_{boost::asio::make_work_guard(io_context_)} {
    const auto count = resolve_worker_count(worker_count);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] {
            // The work_guard keeps run() blocked until stop() is invoked, at
            // which point all handlers are drained and run() returns.
            for (;;) {
                try {
                    io_context_.run();
                    return;
                } catch (...) {
                    // Swallow handler exceptions to keep the worker alive.
                    // Subsystems are expected to surface errors via Result<T>.
                }
            }
        });
    }
}

Executor::~Executor() {
    stop();
}

void Executor::stop() noexcept {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    std::lock_guard<std::mutex> lock{stop_mutex_};
    try {
        work_guard_.reset();
        io_context_.stop();
    } catch (...) {
        // Best effort — io_context::stop is noexcept on Boost 1.84 anyway.
    }
    for (auto& w : workers_) {
        if (w.joinable()) {
            try {
                w.join();
            } catch (...) {
                // Best effort.
            }
        }
    }
    workers_.clear();
}

} // namespace cmlb::core
