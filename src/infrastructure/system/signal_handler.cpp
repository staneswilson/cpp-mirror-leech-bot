// ---------------------------------------------------------------------------
// src/infrastructure/system/signal_handler.cpp
//
// Bridges OS signals to an `asio::cancellation_signal`. Used by `main.cpp`
// to translate Ctrl-C / `systemctl stop` into a cooperative shutdown of
// every co-spawned task.
// ---------------------------------------------------------------------------

#include <csignal>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/system/signal_handler.hpp>

namespace asio = boost::asio;

namespace cmlb::infrastructure::system {

SignalHandler::SignalHandler(asio::any_io_executor exec, asio::cancellation_signal& shutdown)
    : signals_{exec, SIGINT, SIGTERM}, shutdown_{shutdown} {
#if defined(SIGHUP)
    signals_.add(SIGHUP);
#endif
}

asio::awaitable<int> SignalHandler::wait_for_signal() {
    auto [ec, signum] = co_await signals_.async_wait(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        // Cancelled or set was torn down — propagate as -1.
        co_return -1;
    }
    cmlb::core::Logger::info("Received signal {}; shutting down", signum);
    shutdown_.emit(asio::cancellation_type::terminal);
    co_return signum;
}

} // namespace cmlb::infrastructure::system
