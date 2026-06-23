// ---------------------------------------------------------------------------
// rss_subscription.cpp — RssSubscription use case implementation.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <string_view>
#include <utility>

#include <cmlb/application/rss_subscription.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace pers_ns = cmlb::infrastructure::persistence;

RssSubscription::RssSubscription(pers_ns::RssFeedRepository& repo) noexcept : repo_{repo} {
}

asio::awaitable<cmlb::core::Result<std::int64_t>> RssSubscription::add(pers_ns::RssFeed feed) {
    cmlb::core::Logger::info("rss_subscription: add chat={} url={}", feed.chat.value(), feed.url);

    if (feed.url.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument, "rss add: empty URL");
    }
    // Lightweight URL sanity check: must start with http:// or https://. Avoids
    // persisting "hello world"-style typos that would then poll-fail forever.
    {
        constexpr std::string_view kHttp{"http://"};
        constexpr std::string_view kHttps{"https://"};
        const bool ok = feed.url.starts_with(kHttp) || feed.url.starts_with(kHttps);
        if (!ok) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                        "rss add: URL must start with http:// or https://");
        }
    }
    feed.feed_id = 0; // force insert path

    auto saved = co_await repo_.save(std::move(feed));
    if (!saved)
        co_return std::unexpected(saved.error());

    cmlb::core::Logger::info("rss_subscription: added feed_id={}", saved->feed_id);
    co_return saved->feed_id;
}

asio::awaitable<cmlb::core::Result<std::vector<pers_ns::RssFeed>>> RssSubscription::list_for_chat(
    cmlb::domain::ChatId chat) {
    cmlb::core::Logger::info("rss_subscription: list chat={}", chat.value());

    auto all = co_await repo_.list_enabled();
    if (!all)
        co_return std::unexpected(all.error());

    std::vector<pers_ns::RssFeed> filtered;
    filtered.reserve(all->size());
    for (auto& feed : *all) {
        if (feed.chat == chat) {
            filtered.push_back(std::move(feed));
        }
    }
    co_return filtered;
}

asio::awaitable<cmlb::core::Result<void>> RssSubscription::remove(
    std::int64_t feed_id, cmlb::domain::UserId requester, cmlb::domain::ChatId requester_chat) {
    cmlb::core::Logger::info("rss_subscription: remove feed_id={} user={} chat={}",
                             feed_id,
                             requester.value(),
                             requester_chat.value());

    auto loaded = co_await repo_.find(feed_id);
    if (!loaded)
        co_return std::unexpected(loaded.error());
    if (!loaded->has_value()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound, "rss remove: feed not found");
    }
    if (loaded->value().chat != requester_chat) {
        cmlb::core::Logger::warn("rss_subscription: remove denied feed_id={} requester={} "
                                 "feed_chat={} requester_chat={}",
                                 feed_id,
                                 requester.value(),
                                 loaded->value().chat.value(),
                                 requester_chat.value());
        co_return cmlb::core::error(cmlb::core::ErrorCode::PermissionDenied,
                                    "rss remove: requester does not own feed");
    }

    auto removed = co_await repo_.remove(feed_id);
    if (!removed)
        co_return std::unexpected(removed.error());

    cmlb::core::Logger::info("rss_subscription: removed feed_id={}", feed_id);
    co_return cmlb::core::Result<void>{};
}

} // namespace cmlb::application
