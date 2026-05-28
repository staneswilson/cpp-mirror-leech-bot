#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>

/// @file rss_feed_repository.hpp
/// @brief Persistence boundary for RSS feed subscriptions.

namespace cmlb::infrastructure::persistence {

/// One row of the `rss_feeds` table.
///
/// `feed_id` is assigned by SQLite on first insertion. When constructing a
/// record for `save()` leave it at 0; the returned record carries the assigned
/// ID. Existing records (loaded via `find` or `list_enabled`) keep their ID.
struct RssFeed {
    std::int64_t feed_id{0};
    std::string title;
    std::string url;
    domain::ChatId chat;
    std::string include_regex;
    std::string exclude_regex;
    std::string last_guid;
    std::optional<std::chrono::system_clock::time_point> last_checked_at;
    bool enabled{true};
    std::chrono::system_clock::time_point created_at{};
};

class RssFeedRepository {
public:
    virtual ~RssFeedRepository() = default;

    RssFeedRepository() = default;
    RssFeedRepository(const RssFeedRepository&) = delete;
    RssFeedRepository& operator=(const RssFeedRepository&) = delete;
    RssFeedRepository(RssFeedRepository&&) = delete;
    RssFeedRepository& operator=(RssFeedRepository&&) = delete;

    /// Returns every feed where `enabled = 1`.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::vector<RssFeed>>>
    list_enabled() = 0;

    /// Returns the feed identified by @p feed_id, or `nullopt` if no row matches.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::optional<RssFeed>>> find(
        std::int64_t feed_id) = 0;

    /// Persists @p feed. On insert, `feed_id` is assigned by SQLite and the
    /// updated record is returned. On update (when `feed_id != 0`), the same
    /// `feed_id` is preserved. URL conflicts surface as `AlreadyExists`.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<RssFeed>> save(RssFeed feed) = 0;

    /// Updates only the polling-state fields without touching configuration.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> update_state(
        std::int64_t feed_id,
        std::string last_guid,
        std::chrono::system_clock::time_point last_checked_at) = 0;

    /// Removes the row by id. Returns `NotFound` if no row matched.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> remove(
        std::int64_t feed_id) = 0;
};

} // namespace cmlb::infrastructure::persistence
