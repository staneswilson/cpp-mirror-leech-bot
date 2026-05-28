#include <cmlb/infrastructure/rss/rss_feed_poller.hpp>

#include <chrono>
#include <regex>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/cancellation.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>

namespace cmlb::infrastructure::rss {

namespace asio = boost::asio;

namespace {

using cmlb::core::Logger;
using cmlb::infrastructure::persistence::RssFeed;

/// Returns true when the @p text matches the optional regex. An empty
/// (nullopt) regex always matches. Invalid regex patterns are treated as
/// non-matching with a warning so a single broken feed doesn't poison the
/// entire poll cycle.
[[nodiscard]] bool matches_optional_regex(const std::optional<std::string>& pattern,
                                          const std::string& text,
                                          bool default_when_unset) {
    if (!pattern || pattern->empty()) return default_when_unset;
    try {
        std::regex r{*pattern, std::regex::icase};
        return std::regex_search(text, r);
    } catch (const std::regex_error& e) {
        Logger::warn("[rss] invalid regex '{}': {}", *pattern, e.what());
        return false;
    }
}

[[nodiscard]] bool entry_passes_filters(const RssFeed& feed, const RssEntry& entry) {
    // include_regex: when set, must match. When unset, every entry passes.
    if (!matches_optional_regex(feed.include_regex, entry.title, /*default=*/true)) {
        return false;
    }
    // exclude_regex: when set, must NOT match.
    if (matches_optional_regex(feed.exclude_regex, entry.title, /*default=*/false)) {
        return false;
    }
    return true;
}

}  // namespace

RssFeedPoller::RssFeedPoller(cmlb::core::Executor& exec,
                             cmlb::infrastructure::http::BeastHttpClient& http_client,
                             cmlb::infrastructure::persistence::RssFeedRepository& repo,
                             NewEntryHandler on_new_entry)
    : exec_{&exec},
      http_client_{&http_client},
      repo_{&repo},
      on_new_entry_{std::move(on_new_entry)} {}

asio::awaitable<void> RssFeedPoller::run() {
    using namespace std::chrono_literals;

    Logger::info("[rss] feed poller started");

    asio::steady_timer timer{exec_->get_executor()};
    for (;;) {
        // Bail out at the start of every iteration if we've been cancelled.
        const auto cancel_state = co_await asio::this_coro::cancellation_state;
        if (cancel_state.cancelled() != asio::cancellation_type::none) {
            Logger::info("[rss] feed poller cancelled");
            co_return;
        }

        co_await poll_once();

        // Sleep — but stay responsive to cancellation.
        timer.expires_after(1min);
        boost::system::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted) {
            Logger::info("[rss] feed poller cancelled during sleep");
            co_return;
        }
    }
}

asio::awaitable<void> RssFeedPoller::poll_once() {
    auto feeds_result = co_await repo_->list_enabled();
    if (!feeds_result) {
        Logger::warn("[rss] list_enabled failed: {}", feeds_result.error().message);
        co_return;
    }

    // Per-feed check_interval is not in the v1 RssFeed schema; the bot uses a
    // global 5-minute interval. A future migration (V0005) may add a per-feed
    // override. Feeds without `last_checked_at` are polled immediately.
    constexpr auto kDefaultInterval = std::chrono::minutes{5};

    const auto now = std::chrono::system_clock::now();
    for (auto& feed : *feeds_result) {
        if (feed.last_checked_at.has_value()) {
            const auto since = now - *feed.last_checked_at;
            if (since < kDefaultInterval) continue;
        }

        const auto cancel_state = co_await asio::this_coro::cancellation_state;
        if (cancel_state.cancelled() != asio::cancellation_type::none) {
            co_return;
        }

        co_await process_feed(std::move(feed));
    }
}

asio::awaitable<void> RssFeedPoller::process_feed(RssFeed feed) {
    Logger::debug("[rss] polling feed {} ({})", feed.feed_id, feed.url);

    auto http_result = co_await http_client_->get(feed.url);
    if (!http_result) {
        Logger::warn("[rss] feed {} fetch failed: {}", feed.feed_id,
                     http_result.error().message);
        co_return;
    }
    if (http_result->status_code < 200 || http_result->status_code >= 300) {
        Logger::warn("[rss] feed {} returned HTTP {}", feed.feed_id,
                     http_result->status_code);
        co_return;
    }

    auto parsed = RssDocumentParser::parse(http_result->body);
    if (!parsed) {
        Logger::warn("[rss] feed {} parse failed: {}", feed.feed_id,
                     parsed.error().message);
        co_return;
    }

    // `last_guid` is stored as a plain string; empty means "no GUID seen yet".
    const std::string previous_guid = feed.last_guid;
    std::string newest_guid = previous_guid;

    for (const auto& entry : parsed->entries) {
        // GUID ordering: only entries strictly greater than the previously
        // seen GUID are considered new. When no previous GUID has been
        // recorded, every entry is new.
        const bool is_new = previous_guid.empty()
                            || entry.guid > previous_guid;
        if (!is_new) continue;
        if (entry.guid > newest_guid) newest_guid = entry.guid;

        if (!entry_passes_filters(feed, entry)) continue;

        try {
            co_await on_new_entry_(feed, entry);
        } catch (const std::exception& e) {
            Logger::error("[rss] handler threw for feed {}: {}",
                          feed.feed_id, e.what());
        }
    }

    const auto now = std::chrono::system_clock::now();
    auto upd = co_await repo_->update_state(feed.feed_id, newest_guid, now);
    if (!upd) {
        Logger::warn("[rss] update_state failed for feed {}: {}",
                     feed.feed_id, upd.error().message);
    }
}

}  // namespace cmlb::infrastructure::rss
