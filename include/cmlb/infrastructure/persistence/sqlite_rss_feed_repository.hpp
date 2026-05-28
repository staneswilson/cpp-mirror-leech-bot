#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

namespace cmlb::infrastructure::persistence {

class SqliteRssFeedRepository final : public RssFeedRepository {
public:
    explicit SqliteRssFeedRepository(SqliteConnectionPool& pool) noexcept
        : pool_{pool} {}

    [[nodiscard]] boost::asio::awaitable<core::Result<std::vector<RssFeed>>>
    list_enabled() override;

    [[nodiscard]] boost::asio::awaitable<core::Result<std::optional<RssFeed>>>
    find(std::int64_t feed_id) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<RssFeed>>
    save(RssFeed feed) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
    update_state(std::int64_t feed_id,
                 std::string  last_guid,
                 std::chrono::system_clock::time_point last_checked_at) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
    remove(std::int64_t feed_id) override;

private:
    SqliteConnectionPool& pool_;
};

}  // namespace cmlb::infrastructure::persistence
