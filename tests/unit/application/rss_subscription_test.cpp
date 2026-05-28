// ---------------------------------------------------------------------------
// rss_subscription_test.cpp - unit tests for the bundled RSS use case.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmlb/application/rss_subscription.hpp>

#include "in_memory_rss_feed_repository.hpp"
#include "stub_messenger.hpp"

namespace asio = boost::asio;

using cmlb::application::RssSubscription;
using cmlb::core::ErrorCode;
using cmlb::domain::ChatId;
using cmlb::domain::UserId;
using cmlb::infrastructure::persistence::RssFeed;
using cmlb::test_support::InMemoryRssFeedRepository;
using cmlb::test_support::StubMessenger;

namespace {

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

RssFeed sample_feed(std::string url, ChatId chat) {
    return RssFeed{
        .feed_id         = 0,
        .title           = "title",
        .url             = std::move(url),
        .chat            = chat,
        .include_regex   = ".*",
        .exclude_regex   = "",
        .last_guid       = "",
        .last_checked_at = std::nullopt,
        .enabled         = true,
        .created_at      = {},
    };
}

}  // namespace

TEST_CASE("RssSubscription::add assigns an id and persists the feed",
          "[application][rss]") {
    InMemoryRssFeedRepository repo;
    StubMessenger messenger;
    RssSubscription uc{repo, messenger};

    asio::io_context ctx;
    auto added = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<std::int64_t>> {
        co_return co_await uc.add(
            sample_feed("https://example.com/feed.xml", ChatId{-100}));
    });

    REQUIRE(added.has_value());
    CHECK(*added > 0);
    CHECK(repo.size() == 1);
}

TEST_CASE("RssSubscription::list_for_chat filters by chat",
          "[application][rss]") {
    InMemoryRssFeedRepository repo;
    StubMessenger messenger;
    RssSubscription uc{repo, messenger};

    asio::io_context ctx;
    (void)run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<std::int64_t>> {
        co_return co_await uc.add(sample_feed("https://a", ChatId{-1}));
    });
    (void)run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<std::int64_t>> {
        co_return co_await uc.add(sample_feed("https://b", ChatId{-2}));
    });

    auto listed = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<std::vector<RssFeed>>> {
        co_return co_await uc.list_for_chat(ChatId{-2});
    });
    REQUIRE(listed.has_value());
    CHECK(listed->size() == 1);
    CHECK(listed->front().url == "https://b");
}

TEST_CASE("RssSubscription::remove enforces ownership via chat",
          "[application][rss]") {
    InMemoryRssFeedRepository repo;
    StubMessenger messenger;
    RssSubscription uc{repo, messenger};

    asio::io_context ctx;
    auto added = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<std::int64_t>> {
        co_return co_await uc.add(
            sample_feed("https://example.com/feed.xml", ChatId{-100}));
    });
    REQUIRE(added.has_value());

    auto denied = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.remove(*added, UserId{99}, ChatId{-200});
    });
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code == ErrorCode::PermissionDenied);
    CHECK(repo.size() == 1);

    auto ok = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.remove(*added, UserId{99}, ChatId{-100});
    });
    REQUIRE(ok.has_value());
    CHECK(repo.size() == 0);
}

TEST_CASE("RssSubscription::remove returns NotFound for unknown ids",
          "[application][rss]") {
    InMemoryRssFeedRepository repo;
    StubMessenger messenger;
    RssSubscription uc{repo, messenger};

    asio::io_context ctx;
    auto result = run_on(ctx, [&]() -> asio::awaitable<
        cmlb::core::Result<void>> {
        co_return co_await uc.remove(424242, UserId{1}, ChatId{-1});
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::NotFound);
}
