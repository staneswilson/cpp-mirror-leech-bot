#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/signal_set.hpp>

namespace cmlb::infrastructure::system {

/// Installs OS signal handlers and forwards them to a `cancellation_signal`.
///
/// Registers `SIGINT` and `SIGTERM` on every platform, plus `SIGHUP` on
/// POSIX (no-op on Windows where the signal does not exist).
///
/// On the first signal received the handler:
///   1. Logs an info-level message identifying the signal.
///   2. Emits `boost::asio::cancellation_type::terminal` on the supplied
///      shutdown signal so co-spawned tasks unwind cleanly.
///
/// The handler does *not* call `std::_Exit` or otherwise terminate the
/// process — orderly shutdown is the caller's responsibility once the
/// cancellation propagates.
class SignalHandler {
public:
    /// Constructs the handler and immediately registers `SIGINT`/`SIGTERM`
    /// (and `SIGHUP` on POSIX) against `exec`. `shutdown` must outlive this
    /// object since the handler keeps a reference to it.
    SignalHandler(boost::asio::any_io_executor exec, boost::asio::cancellation_signal& shutdown);

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
    SignalHandler(SignalHandler&&) = delete;
    SignalHandler& operator=(SignalHandler&&) = delete;

    ~SignalHandler() = default;

    /// Awaitable that completes when the first registered signal arrives.
    /// Returns the signal number (e.g. `SIGINT == 2`). Subsequent signals
    /// are ignored once one has fired; reinstall the handler if needed.
    [[nodiscard]] boost::asio::awaitable<int> wait_for_signal();

private:
    boost::asio::signal_set signals_;
    boost::asio::cancellation_signal& shutdown_;
};

} // namespace cmlb::infrastructure::system
