#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>

namespace cmlb::test_support {

class InMemoryRssFeedRepository final
    : public cmlb::infrastructure::persistence::RssFeedRepository {
public:
    InMemoryRssFeedRepository()  = default;
    ~InMemoryRssFeedRepository() override = default;

    boost::asio::awaitable<cmlb::core::Result<
        std::vector<cmlb::infrastructure::persistence::RssFeed>>>
    list_enabled() override {
        std::lock_guard lk{mutex_};
        std::vector<cmlb::infrastructure::persistence::RssFeed> out;
        for (const auto& [_, feed] : store_) {
            if (feed.enabled) out.push_back(feed);
        }
        co_return out;
    }

    boost::asio::awaitable<cmlb::core::Result<
        std::optional<cmlb::infrastructure::persistence::RssFeed>>>
    find(std::int64_t feed_id) override {
        std::lock_guard lk{mutex_};
        auto it = store_.find(feed_id);
        if (it == store_.end()) {
            co_return std::optional<
                cmlb::infrastructure::persistence::RssFeed>{};
        }
        co_return std::optional<
            cmlb::infrastructure::persistence::RssFeed>{it->second};
    }

    boost::asio::awaitable<cmlb::core::Result<
        cmlb::infrastructure::persistence::RssFeed>>
    save(cmlb::infrastructure::persistence::RssFeed feed) override {
        std::lock_guard lk{mutex_};
        if (feed.feed_id == 0) {
            // Reject duplicate URL inserts to mirror the SQLite UNIQUE
            // constraint surfaced via AlreadyExists.
            for (const auto& [_, existing] : store_) {
                if (existing.url == feed.url) {
                    co_return cmlb::core::error(
                        cmlb::core::ErrorCode::AlreadyExists,
                        "feed URL already present");
                }
            }
            feed.feed_id    = ++next_id_;
            feed.created_at = std::chrono::system_clock::now();
        }
        store_.insert_or_assign(feed.feed_id, feed);
        co_return feed;
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    update_state(std::int64_t feed_id,
                 std::string last_guid,
                 std::chrono::system_clock::time_point last_checked_at)
        override {
        std::lock_guard lk{mutex_};
        auto it = store_.find(feed_id);
        if (it == store_.end()) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                        "feed not found");
        }
        it->second.last_guid       = std::move(last_guid);
        it->second.last_checked_at = last_checked_at;
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>>
    remove(std::int64_t feed_id) override {
        std::lock_guard lk{mutex_};
        if (store_.erase(feed_id) == 0) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                        "feed not found");
        }
        co_return cmlb::core::Result<void>{};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lk{mutex_};
        return store_.size();
    }

private:
    mutable std::mutex mutex_;
    std::int64_t next_id_{0};
    std::map<std::int64_t, cmlb::infrastructure::persistence::RssFeed> store_;
};

}  // namespace cmlb::test_support
