#pragma once

#include <functional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/infrastructure/rss/rss_document_parser.hpp>

namespace cmlb::core {
class Executor;
}  // namespace cmlb::core

namespace cmlb::infrastructure::http {
class BeastHttpClient;
}  // namespace cmlb::infrastructure::http

namespace cmlb::infrastructure::persistence {
struct RssFeed;
class RssFeedRepository;
}  // namespace cmlb::infrastructure::persistence

namespace cmlb::infrastructure::rss {

/// Long-lived coroutine that periodically polls every enabled RSS feed in
/// the persistence layer, filters new entries through the per-feed
/// include/exclude regular expressions, and dispatches matches via a
/// caller-supplied callback. Cancellation aware: the loop terminates when
/// the bound coroutine's cancellation signal fires.
class RssFeedPoller {
public:
    /// Async callback fired once per new matching entry. The poller awaits
    /// the callback so handlers can apply back-pressure (e.g. queue depth).
    using NewEntryHandler = std::function<
        boost::asio::awaitable<void>(cmlb::infrastructure::persistence::RssFeed,
                                     RssEntry)>;

    RssFeedPoller(cmlb::core::Executor& exec,
                  cmlb::infrastructure::http::BeastHttpClient& http_client,
                  cmlb::infrastructure::persistence::RssFeedRepository& repo,
                  NewEntryHandler on_new_entry);

    ~RssFeedPoller() = default;

    RssFeedPoller(const RssFeedPoller&)            = delete;
    RssFeedPoller& operator=(const RssFeedPoller&) = delete;
    RssFeedPoller(RssFeedPoller&&)                 = delete;
    RssFeedPoller& operator=(RssFeedPoller&&)      = delete;

    /// Main loop. Sleeps 1 minute between scans; respects co_await
    /// `this_coro::cancellation_state()` so a parent `co_spawn` with a
    /// cancellation slot can drive a clean shutdown.
    boost::asio::awaitable<void> run();

private:
    boost::asio::awaitable<void> poll_once();
    boost::asio::awaitable<void> process_feed(
        cmlb::infrastructure::persistence::RssFeed feed);

    cmlb::core::Executor*                                exec_;
    cmlb::infrastructure::http::BeastHttpClient*         http_client_;
    cmlb::infrastructure::persistence::RssFeedRepository* repo_;
    NewEntryHandler                                      on_new_entry_;
};

}  // namespace cmlb::infrastructure::rss
