#include "utils/rss_monitor.hpp"
#include "core/logger.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <random>
#include <sstream>
#include <regex>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace cmlb {

// ============================================================================
// Simple XML Parser for RSS/Atom
// ============================================================================

class SimpleXmlParser {
public:
    static std::vector<RssItem> parseRss(const std::string& xml) {
        std::vector<RssItem> items;
        
        // Find all <item> elements (RSS 2.0)
        std::regex item_regex(R"(<item[^>]*>([\s\S]*?)</item>)", std::regex::icase);
        auto items_begin = std::sregex_iterator(xml.begin(), xml.end(), item_regex);
        auto items_end = std::sregex_iterator();
        
        for (auto it = items_begin; it != items_end; ++it) {
            std::string item_content = (*it)[1].str();
            RssItem item;
            
            item.title = extractTag(item_content, "title");
            item.link = extractTag(item_content, "link");
            item.description = extractTag(item_content, "description");
            item.guid = extractTagOpt(item_content, "guid");
            
            // Try to extract enclosure URL (for torrents)
            std::regex enclosure_regex(R"(<enclosure[^>]*url="([^"]*)"[^>]*/?>)", std::regex::icase);
            std::smatch enclosure_match;
            if (std::regex_search(item_content, enclosure_match, enclosure_regex)) {
                std::string url = enclosure_match[1].str();
                if (url.find(".torrent") != std::string::npos) {
                    item.torrent_url = url;
                }
            }
            
            // Try to extract magnet link
            std::regex magnet_regex(R"(magnet:\?[^<\s"]+)");
            std::smatch magnet_match;
            if (std::regex_search(item_content, magnet_match, magnet_regex)) {
                item.magnet = magnet_match[0].str();
            }
            
            // Parse pubDate
            std::string pub_date = extractTag(item_content, "pubDate");
            if (!pub_date.empty()) {
                item.pub_date = parseRfc822Date(pub_date);
            }
            
            // Try to extract content length
            std::regex size_regex(R"(length="(\d+)")", std::regex::icase);
            std::smatch size_match;
            if (std::regex_search(item_content, size_match, size_regex)) {
                item.size = std::stoll(size_match[1].str());
            }
            
            items.push_back(std::move(item));
        }
        
        // If no RSS items, try Atom format
        if (items.empty()) {
            items = parseAtom(xml);
        }
        
        return items;
    }
    
    static std::vector<RssItem> parseAtom(const std::string& xml) {
        std::vector<RssItem> items;
        
        std::regex entry_regex(R"(<entry[^>]*>([\s\S]*?)</entry>)", std::regex::icase);
        auto entries_begin = std::sregex_iterator(xml.begin(), xml.end(), entry_regex);
        auto entries_end = std::sregex_iterator();
        
        for (auto it = entries_begin; it != entries_end; ++it) {
            std::string entry_content = (*it)[1].str();
            RssItem item;
            
            item.title = extractTag(entry_content, "title");
            item.description = extractTag(entry_content, "summary");
            if (item.description.empty()) {
                item.description = extractTag(entry_content, "content");
            }
            item.guid = extractTagOpt(entry_content, "id");
            
            // Extract link from href attribute
            std::regex link_regex(R"(<link[^>]*href="([^"]*)"[^>]*/?>)", std::regex::icase);
            std::smatch link_match;
            if (std::regex_search(entry_content, link_match, link_regex)) {
                item.link = link_match[1].str();
            }
            
            // Parse updated date
            std::string updated = extractTag(entry_content, "updated");
            if (!updated.empty()) {
                item.pub_date = parseIso8601Date(updated);
            }
            
            items.push_back(std::move(item));
        }
        
        return items;
    }

private:
    static std::string extractTag(const std::string& xml, const std::string& tag) {
        std::regex tag_regex("<" + tag + "[^>]*>([\\s\\S]*?)</" + tag + ">", std::regex::icase);
        std::smatch match;
        if (std::regex_search(xml, match, tag_regex)) {
            std::string content = match[1].str();
            // Remove CDATA wrapper if present
            std::regex cdata_regex(R"(<!\[CDATA\[([\s\S]*?)\]\]>)");
            std::smatch cdata_match;
            if (std::regex_search(content, cdata_match, cdata_regex)) {
                return cdata_match[1].str();
            }
            // Decode basic HTML entities
            return decodeHtmlEntities(content);
        }
        return "";
    }
    
    static std::optional<std::string> extractTagOpt(const std::string& xml, const std::string& tag) {
        std::string result = extractTag(xml, tag);
        if (result.empty()) return std::nullopt;
        return result;
    }
    
    static std::string decodeHtmlEntities(const std::string& str) {
        std::string result = str;
        static const std::vector<std::pair<std::string, std::string>> entities = {
            {"&amp;", "&"},
            {"&lt;", "<"},
            {"&gt;", ">"},
            {"&quot;", "\""},
            {"&apos;", "'"},
            {"&#39;", "'"}
        };
        for (const auto& [entity, replacement] : entities) {
            size_t pos = 0;
            while ((pos = result.find(entity, pos)) != std::string::npos) {
                result.replace(pos, entity.length(), replacement);
                pos += replacement.length();
            }
        }
        return result;
    }
    
    static std::chrono::system_clock::time_point parseRfc822Date(const std::string& date) {
        // Simplified RFC 822 date parsing
        // Example: "Mon, 01 Jan 2024 12:00:00 GMT"
        std::tm tm = {};
        std::istringstream ss(date);
        ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");
        if (ss.fail()) {
            return std::chrono::system_clock::now();
        }
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
    
    static std::chrono::system_clock::time_point parseIso8601Date(const std::string& date) {
        // Simplified ISO 8601 parsing
        // Example: "2024-01-01T12:00:00Z"
        std::tm tm = {};
        std::istringstream ss(date);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (ss.fail()) {
            return std::chrono::system_clock::now();
        }
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
};

// ============================================================================
// HTTP Client for fetching feeds
// ============================================================================

class HttpClient {
public:
    static Result<std::string> fetch(const std::string& url) {
        try {
            // Parse URL
            std::regex url_regex(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?$)");
            std::smatch match;
            if (!std::regex_match(url, match, url_regex)) {
                return std::unexpected(AppError(ErrorCode::InvalidArgument, "Invalid URL format"));
            }
            
            std::string scheme = match[1].str();
            std::string host = match[2].str();
            std::string port = match[3].matched ? match[3].str() : (scheme == "https" ? "443" : "80");
            std::string path = match[4].matched ? match[4].str() : "/";
            
            bool use_ssl = (scheme == "https");
            
            net::io_context ioc;
            
            if (use_ssl) {
                return fetchHttps(ioc, host, port, path);
            } else {
                return fetchHttp(ioc, host, port, path);
            }
            
        } catch (const std::exception& e) {
            return std::unexpected(AppError(ErrorCode::NetworkError, e.what()));
        }
    }

private:
    static Result<std::string> fetchHttp(net::io_context& ioc, 
                                          const std::string& host,
                                          const std::string& port,
                                          const std::string& path) {
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        
        auto const results = resolver.resolve(host, port);
        stream.connect(results);
        
        http::request<http::string_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "CMLB-Bot/0.2.0");
        req.set(http::field::accept, "application/rss+xml, application/atom+xml, application/xml, text/xml");
        
        http::write(stream, req);
        
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        
        if (res.result() != http::status::ok) {
            return std::unexpected(AppError(ErrorCode::NetworkError, 
                "HTTP " + std::to_string(static_cast<int>(res.result()))));
        }
        
        return res.body();
    }
    
    static Result<std::string> fetchHttps(net::io_context& ioc,
                                           const std::string& host,
                                           const std::string& port,
                                           const std::string& path) {
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
        
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        
        // Set SNI hostname
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            return std::unexpected(AppError(ErrorCode::NetworkError, "Failed to set SNI hostname"));
        }
        
        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).connect(results);
        
        stream.handshake(ssl::stream_base::client);
        
        http::request<http::string_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "CMLB-Bot/0.2.0");
        req.set(http::field::accept, "application/rss+xml, application/atom+xml, application/xml, text/xml");
        
        http::write(stream, req);
        
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        
        beast::error_code ec;
        stream.shutdown(ec);
        
        if (res.result() != http::status::ok) {
            return std::unexpected(AppError(ErrorCode::NetworkError,
                "HTTP " + std::to_string(static_cast<int>(res.result()))));
        }
        
        return res.body();
    }
};

// ============================================================================
// RssMonitor Implementation
// ============================================================================

class RssMonitor::Impl {
public:
    std::unordered_map<std::string, RssFeed> feeds_;
    mutable std::shared_mutex mutex_;
    
    RssItemCallback callback_;
    
    std::jthread monitor_thread_;
    std::atomic<bool> running_{false};
    std::condition_variable_any cv_;

    std::string generateFeedId() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<uint64_t> dis;
        
        return std::format("rss_{:08x}", dis(gen) & 0xFFFFFFFF);
    }

    bool matchesFilters(const RssFeed& feed, const RssItem& item) {
        // Check include patterns
        if (!feed.include_patterns.empty()) {
            bool matches_any = false;
            for (const auto& pattern : feed.include_patterns) {
                try {
                    std::regex re(pattern, std::regex::icase);
                    if (std::regex_search(item.title, re)) {
                        matches_any = true;
                        break;
                    }
                } catch (const std::regex_error&) {
                    // Invalid regex, skip
                }
            }
            if (!matches_any) return false;
        }
        
        // Check exclude patterns
        for (const auto& pattern : feed.exclude_patterns) {
            try {
                std::regex re(pattern, std::regex::icase);
                if (std::regex_search(item.title, re)) {
                    return false;
                }
            } catch (const std::regex_error&) {
                // Invalid regex, skip
            }
        }
        
        return true;
    }

    void monitorLoop(std::stop_token stoken) {
        Logger::info("RSS monitor loop started");
        
        while (!stoken.stop_requested()) {
            auto now = std::chrono::system_clock::now();
            
            std::vector<RssFeed> feeds_to_check;
            {
                std::shared_lock lock(mutex_);
                for (const auto& [id, feed] : feeds_) {
                    if (!feed.enabled || feed.paused) continue;
                    
                    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
                        now - feed.last_checked);
                    
                    if (elapsed.count() >= feed.check_interval_min) {
                        feeds_to_check.push_back(feed);
                    }
                }
            }
            
            for (auto& feed : feeds_to_check) {
                Logger::debug("Checking RSS feed: {}", feed.name);
                
                auto result = RssMonitor::parseFeed(feed.url);
                if (!result) {
                    Logger::warn("Failed to parse RSS feed {}: {}", 
                                 feed.name, result.error().message);
                    continue;
                }
                
                int new_items = 0;
                for (const auto& item : *result) {
                    // Skip already seen items
                    if (feed.last_guid && item.guid == *feed.last_guid) {
                        break;
                    }
                    
                    if (matchesFilters(feed, item)) {
                        Logger::info("RSS match: '{}' in feed '{}'", item.title, feed.name);
                        
                        if (callback_) {
                            callback_(feed, item);
                        }
                        new_items++;
                    }
                }
                
                // Update last checked and last guid
                {
                    std::unique_lock lock(mutex_);
                    auto it = feeds_.find(feed.id);
                    if (it != feeds_.end()) {
                        it->second.last_checked = now;
                        if (!result->empty() && result->front().guid) {
                            it->second.last_guid = result->front().guid;
                        }
                    }
                }
                
                if (new_items > 0) {
                    Logger::info("Processed {} new items from feed '{}'", new_items, feed.name);
                }
            }
            
            // Wait for next check cycle (1 minute intervals)
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::minutes(1), [&] {
                return stoken.stop_requested();
            });
        }
        
        Logger::info("RSS monitor loop stopped");
    }
};

RssMonitor::RssMonitor() : impl_(std::make_unique<Impl>()) {
    Logger::info("RssMonitor initialized");
}

RssMonitor::~RssMonitor() {
    stopMonitoring();
}

std::string RssMonitor::addFeed(const RssFeed& feed) {
    std::unique_lock lock(impl_->mutex_);
    
    RssFeed new_feed = feed;
    new_feed.id = impl_->generateFeedId();
    new_feed.created_at = std::chrono::system_clock::now();
    new_feed.last_checked = std::chrono::system_clock::time_point{};
    
    impl_->feeds_[new_feed.id] = new_feed;
    
    Logger::info("Added RSS feed: {} ({})", new_feed.name, new_feed.id);
    return new_feed.id;
}

void RssMonitor::removeFeed(const std::string& feed_id) {
    std::unique_lock lock(impl_->mutex_);
    impl_->feeds_.erase(feed_id);
    Logger::info("Removed RSS feed: {}", feed_id);
}

std::optional<RssFeed> RssMonitor::getFeed(const std::string& feed_id) const {
    std::shared_lock lock(impl_->mutex_);
    auto it = impl_->feeds_.find(feed_id);
    if (it != impl_->feeds_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<RssFeed> RssMonitor::getUserFeeds(int64_t user_id) const {
    std::shared_lock lock(impl_->mutex_);
    std::vector<RssFeed> result;
    for (const auto& [id, feed] : impl_->feeds_) {
        if (feed.user_id == user_id) {
            result.push_back(feed);
        }
    }
    return result;
}

std::vector<RssFeed> RssMonitor::getAllFeeds() const {
    std::shared_lock lock(impl_->mutex_);
    std::vector<RssFeed> result;
    for (const auto& [id, feed] : impl_->feeds_) {
        result.push_back(feed);
    }
    return result;
}

void RssMonitor::updateFeed(const RssFeed& feed) {
    std::unique_lock lock(impl_->mutex_);
    auto it = impl_->feeds_.find(feed.id);
    if (it != impl_->feeds_.end()) {
        it->second = feed;
    }
}

void RssMonitor::setPaused(const std::string& feed_id, bool paused) {
    std::unique_lock lock(impl_->mutex_);
    auto it = impl_->feeds_.find(feed_id);
    if (it != impl_->feeds_.end()) {
        it->second.paused = paused;
        Logger::info("RSS feed {} {}", feed_id, paused ? "paused" : "resumed");
    }
}

void RssMonitor::setCallback(RssItemCallback callback) {
    impl_->callback_ = std::move(callback);
}

Result<std::vector<RssItem>> RssMonitor::checkFeed(const std::string& feed_id) {
    std::optional<RssFeed> feed;
    {
        std::shared_lock lock(impl_->mutex_);
        auto it = impl_->feeds_.find(feed_id);
        if (it != impl_->feeds_.end()) {
            feed = it->second;
        }
    }
    
    if (!feed) {
        return std::unexpected(AppError(ErrorCode::NotFound, "Feed not found"));
    }
    
    return parseFeed(feed->url);
}

void RssMonitor::startMonitoring() {
    if (impl_->running_.exchange(true)) return;
    
    impl_->monitor_thread_ = std::jthread([this](std::stop_token stoken) {
        impl_->monitorLoop(stoken);
    });
    
    Logger::info("RSS monitoring started");
}

void RssMonitor::stopMonitoring() {
    impl_->running_.store(false);
    impl_->cv_.notify_all();
    
    if (impl_->monitor_thread_.joinable()) {
        impl_->monitor_thread_.request_stop();
    }
    
    Logger::info("RSS monitoring stopped");
}

Result<std::vector<RssItem>> RssMonitor::parseFeed(const std::string& url) {
    // Fetch the feed content
    auto fetch_result = HttpClient::fetch(url);
    if (!fetch_result) {
        return std::unexpected(fetch_result.error());
    }
    
    // Parse the XML
    auto items = SimpleXmlParser::parseRss(*fetch_result);
    
    Logger::debug("Parsed {} items from feed: {}", items.size(), url);
    return items;
}

} // namespace cmlb
