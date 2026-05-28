#include <cmlb/infrastructure/persistence/sqlite_rss_feed_repository.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>

#include "time_codec.hpp"

namespace cmlb::infrastructure::persistence {

namespace {

using detail::parse_iso8601;
using detail::to_iso8601;

[[nodiscard]] core::Result<RssFeed> map_row(std::int64_t                 feed_id,
                                            std::string                  title,
                                            std::string                  url,
                                            std::int64_t                 chat_id,
                                            std::string                  include_re,
                                            std::string                  exclude_re,
                                            std::string                  last_guid,
                                            std::unique_ptr<std::string> last_checked_at,
                                            int                          enabled,
                                            std::string                  created_at) {
    auto created_tp = parse_iso8601(created_at);
    if (!created_tp) {
        return std::unexpected{created_tp.error()};
    }
    std::optional<std::chrono::system_clock::time_point> last_checked;
    if (last_checked_at) {
        auto parsed = parse_iso8601(*last_checked_at);
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        last_checked = *parsed;
    }
    return RssFeed{
        .feed_id          = feed_id,
        .title            = std::move(title),
        .url              = std::move(url),
        .chat             = domain::ChatId{chat_id},
        .include_regex    = std::move(include_re),
        .exclude_regex    = std::move(exclude_re),
        .last_guid        = std::move(last_guid),
        .last_checked_at  = last_checked,
        .enabled          = enabled != 0,
        .created_at       = *created_tp,
    };
}

}  // namespace

boost::asio::awaitable<core::Result<std::vector<RssFeed>>>
SqliteRssFeedRepository::list_enabled() {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        std::vector<RssFeed>  out;
        core::Result<void>    status{};

        db << R"SQL(
            SELECT feed_id, title, url, chat_id, include_regex, exclude_regex,
                   last_guid, last_checked_at, enabled, created_at
              FROM rss_feeds
             WHERE enabled = 1
             ORDER BY feed_id ASC;
        )SQL"
           >> [&](std::int64_t                 feed_id,
                  std::string                  title,
                  std::string                  url,
                  std::int64_t                 chat_id,
                  std::string                  include_re,
                  std::string                  exclude_re,
                  std::string                  last_guid,
                  std::unique_ptr<std::string> last_checked_at,
                  int                          enabled,
                  std::string                  created_at) {
               if (!status.has_value()) return;
               auto row = map_row(feed_id, std::move(title), std::move(url), chat_id,
                                  std::move(include_re), std::move(exclude_re),
                                  std::move(last_guid), std::move(last_checked_at),
                                  enabled, std::move(created_at));
               if (!row) {
                   status = std::unexpected{row.error()};
                   return;
               }
               out.push_back(std::move(*row));
           };

        if (!status.has_value()) {
            co_return std::unexpected{status.error()};
        }
        co_return out;
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.list_enabled failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.list_enabled threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<std::optional<RssFeed>>>
SqliteRssFeedRepository::find(std::int64_t feed_id) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        std::optional<RssFeed> result;
        core::Result<void>     status{};

        db << R"SQL(
            SELECT feed_id, title, url, chat_id, include_regex, exclude_regex,
                   last_guid, last_checked_at, enabled, created_at
              FROM rss_feeds
             WHERE feed_id = ?;
        )SQL"
           << feed_id
           >> [&](std::int64_t                 row_id,
                  std::string                  title,
                  std::string                  url,
                  std::int64_t                 chat_id,
                  std::string                  include_re,
                  std::string                  exclude_re,
                  std::string                  last_guid,
                  std::unique_ptr<std::string> last_checked_at,
                  int                          enabled,
                  std::string                  created_at) {
               auto row = map_row(row_id, std::move(title), std::move(url), chat_id,
                                  std::move(include_re), std::move(exclude_re),
                                  std::move(last_guid), std::move(last_checked_at),
                                  enabled, std::move(created_at));
               if (!row) {
                   status = std::unexpected{row.error()};
                   return;
               }
               result = std::move(*row);
           };

        if (!status.has_value()) {
            co_return std::unexpected{status.error()};
        }
        co_return result;
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.find failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.find threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<RssFeed>>
SqliteRssFeedRepository::save(RssFeed feed) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    const auto now = std::chrono::system_clock::now();
    if (feed.created_at.time_since_epoch().count() == 0) {
        feed.created_at = now;
    }

    try {
        auto bind_optional_time =
            [](sqlite::database_binder& binder,
               const std::optional<std::chrono::system_clock::time_point>& tp) {
                if (tp.has_value()) {
                    binder << to_iso8601(*tp);
                } else {
                    binder << nullptr;
                }
            };

        if (feed.feed_id == 0) {
            auto stmt = db << R"SQL(
                INSERT INTO rss_feeds
                    (title, url, chat_id, include_regex, exclude_regex,
                     last_guid, last_checked_at, enabled, created_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
            )SQL";
            stmt << feed.title << feed.url << feed.chat.value()
                 << feed.include_regex << feed.exclude_regex
                 << feed.last_guid;
            bind_optional_time(stmt, feed.last_checked_at);
            stmt << (feed.enabled ? 1 : 0) << to_iso8601(feed.created_at);
            stmt.execute();
            feed.feed_id = sqlite3_last_insert_rowid(db.connection().get());
        } else {
            auto stmt = db << R"SQL(
                INSERT OR REPLACE INTO rss_feeds
                    (feed_id, title, url, chat_id, include_regex, exclude_regex,
                     last_guid, last_checked_at, enabled, created_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
            )SQL";
            stmt << feed.feed_id << feed.title << feed.url << feed.chat.value()
                 << feed.include_regex << feed.exclude_regex
                 << feed.last_guid;
            bind_optional_time(stmt, feed.last_checked_at);
            stmt << (feed.enabled ? 1 : 0) << to_iso8601(feed.created_at);
            stmt.execute();
        }
        co_return feed;
    } catch (const sqlite::sqlite_exception& ex) {
        // SQLITE_CONSTRAINT_UNIQUE → 2067; map to AlreadyExists.
        const int code = ex.get_code();
        if (code == SQLITE_CONSTRAINT || code == 2067 /* SQLITE_CONSTRAINT_UNIQUE */) {
            co_return core::error(
                core::ErrorCode::AlreadyExists,
                std::string{"rss_feeds.save: URL already exists: "} + feed.url);
        }
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.save failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.save threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>>
SqliteRssFeedRepository::update_state(std::int64_t feed_id,
                                       std::string  last_guid,
                                       std::chrono::system_clock::time_point last_checked_at) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        db << R"SQL(
            UPDATE rss_feeds
               SET last_guid = ?, last_checked_at = ?
             WHERE feed_id = ?;
        )SQL"
           << last_guid << to_iso8601(last_checked_at) << feed_id;

        if (sqlite3_changes(db.connection().get()) == 0) {
            co_return core::error(
                core::ErrorCode::NotFound,
                "rss_feeds.update_state: no row for feed_id "
                    + std::to_string(feed_id));
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.update_state failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.update_state threw: "} + ex.what());
    }
}

boost::asio::awaitable<core::Result<void>>
SqliteRssFeedRepository::remove(std::int64_t feed_id) {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    try {
        db << "DELETE FROM rss_feeds WHERE feed_id = ?;" << feed_id;
        if (sqlite3_changes(db.connection().get()) == 0) {
            co_return core::error(
                core::ErrorCode::NotFound,
                "rss_feeds.remove: no row for feed_id " + std::to_string(feed_id));
        }
        co_return core::Result<void>{};
    } catch (const sqlite::sqlite_exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.remove failed: "} + ex.what());
    } catch (const std::exception& ex) {
        co_return core::error(core::ErrorCode::Database,
                              std::string{"rss_feeds.remove threw: "} + ex.what());
    }
}

}  // namespace cmlb::infrastructure::persistence
