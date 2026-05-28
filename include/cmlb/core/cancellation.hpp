#pragma once

#include <chrono>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <cmlb/core/error.hpp>

namespace cmlb::core {

/// RAII wrapper around `asio::cancellation_signal` for ergonomic stop
/// propagation. Bind the slot via `co_spawn(..., bind_cancellation_slot(...))`
/// or `asio::bind_cancellation_slot` on awaitables.
class StopSource {
public:
    StopSource() = default;
    ~StopSource() = default;

    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    /// Fires the cancellation signal at the requested level (default: all).
    void request_stop(
        boost::asio::cancellation_type type = boost::asio::cancellation_type::all) noexcept {
        signal_.emit(type);
    }

    /// Returns a slot suitable for `bind_cancellation_slot`.
    [[nodiscard]] boost::asio::cancellation_slot get_token() noexcept {
        return signal_.slot();
    }

private:
    boost::asio::cancellation_signal signal_;
};

/// Awaits `op` with a deadline. If the timer fires first, returns
/// `error(ErrorCode::Timeout, ...)`. Otherwise propagates `op`'s result.
///
/// Implementation lives in the header because of the function template.
template <typename T>
[[nodiscard]] boost::asio::awaitable<Result<T>> with_timeout(boost::asio::awaitable<Result<T>> op,
                                                             std::chrono::milliseconds timeout) {
    namespace asio = boost::asio;
    using namespace boost::asio::experimental::awaitable_operators;

    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer{executor};
    timer.expires_after(timeout);

    auto result = co_await (std::move(op) || timer.async_wait(asio::use_awaitable));

    if (result.index() == 0) {
        // Operation completed first.
        co_return std::get<0>(std::move(result));
    }
    co_return error(ErrorCode::Timeout,
                    "operation exceeded " + std::to_string(timeout.count()) + "ms deadline");
}

/// Awaits any of `signals` (e.g. `SIGINT`, `SIGTERM`) on the given executor.
/// Returns when the first signal arrives. Intended for the main shutdown
/// hook: spawn this alongside the main loop and propagate cancellation when
/// it returns.
[[nodiscard]] boost::asio::awaitable<void> cancel_on_signal(boost::asio::any_io_executor exec,
                                                            std::span<const int> signals);

} // namespace cmlb::core
