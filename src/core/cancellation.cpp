#include <cmlb/core/cancellation.hpp>

#include <span>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace cmlb::core {

boost::asio::awaitable<void> cancel_on_signal(
    boost::asio::any_io_executor exec,
    std::span<const int> signals) {
    boost::asio::signal_set sigs{exec};
    for (const int s : signals) {
        sigs.add(s);
    }
    co_await sigs.async_wait(boost::asio::use_awaitable);
    co_return;
}

}  // namespace cmlb::core
