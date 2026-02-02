#ifndef CMLB_UTILS_RSS_MONITOR_HPP
#define CMLB_UTILS_RSS_MONITOR_HPP

#include "core/types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <regex>
#include <functional>

namespace cmlb {

/**
 * @brief RSS feed item.
 */
struct RssItem {
    std::string title;
    std::string link;
    std::string description;
    std::optional<std::string> guid;
    std::optional<std::string> magnet;           // Torrent magnet link if available
    std::optional<std::string> torrent_url;      // Direct torrent URL
    std::optional<int64_t> size;
    std::chrono::system_clock::time_point pub_date;
};

/**
 * @brief RSS feed subscription with filters.
 */
struct RssFeed {
    std::string id;
    std::string url;
    std::string name;
    int64_t user_id;
    int64_t chat_id;
    
    bool enabled{true};
    bool paused{false};
    
    // Filters
    std::vector<std::string> include_patterns;   // Regex patterns to include
    std::vector<std::string> exclude_patterns;   // Regex patterns to exclude
    
    // Command to run for matching items
    std::string command{"mirror"};               // mirror, leech, qbmirror, etc.
    
    // State
    std::optional<std::string> last_guid;
    std::chrono::system_clock::time_point last_checked;
    std::chrono::system_clock::time_point created_at;
    
    int check_interval_min{15};                  // Minutes between checks
};

/**
 * @brief Callback when new RSS item matches filters.
 */
using RssItemCallback = std::function<void(const RssFeed& feed, const RssItem& item)>;

/**
 * @brief RSS feed monitor.
 * 
 * Features:
 * - Multiple feed subscriptions
 * - Regex-based title filtering
 * - Auto-download on match
 * - Configurable check intervals
 */
class RssMonitor {
public:
    RssMonitor();
    ~RssMonitor();

    /**
     * @brief Add a new feed subscription.
     * @return Feed ID
     */
    std::string addFeed(const RssFeed& feed);

    /**
     * @brief Remove a feed subscription.
     */
    void removeFeed(const std::string& feed_id);

    /**
     * @brief Get a feed by ID.
     */
    std::optional<RssFeed> getFeed(const std::string& feed_id) const;

    /**
     * @brief Get all feeds for a user.
     */
    std::vector<RssFeed> getUserFeeds(int64_t user_id) const;

    /**
     * @brief Get all active feeds.
     */
    std::vector<RssFeed> getAllFeeds() const;

    /**
     * @brief Update feed configuration.
     */
    void updateFeed(const RssFeed& feed);

    /**
     * @brief Pause/resume a feed.
     */
    void setPaused(const std::string& feed_id, bool paused);

    /**
     * @brief Set callback for new matching items.
     */
    void setCallback(RssItemCallback callback);

    /**
     * @brief Manually check a feed for new items.
     * @return New items found
     */
    Result<std::vector<RssItem>> checkFeed(const std::string& feed_id);

    /**
     * @brief Start automatic monitoring thread.
     */
    void startMonitoring();

    /**
     * @brief Stop automatic monitoring.
     */
    void stopMonitoring();

    /**
     * @brief Parse RSS/Atom feed from URL.
     */
    static Result<std::vector<RssItem>> parseFeed(const std::string& url);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cmlb

#endif // CMLB_UTILS_RSS_MONITOR_HPP
