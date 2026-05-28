// ---------------------------------------------------------------------------
// src/infrastructure/download/qbittorrent_downloader.cpp
//
// qBittorrent Web API v2 adapter. Wraps the shared BeastHttpClient, handles
// cookie-based session auth, and translates JSON payloads into our normalized
// DownloadStatus model.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <nlohmann/json.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/download/qbittorrent_downloader.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>

namespace cmlb::infrastructure::download {

namespace {

namespace http = cmlb::infrastructure::http;
using json = nlohmann::json;
using cmlb::core::error;
using cmlb::core::ErrorCode;
using cmlb::core::Result;
using cmlb::domain::Gid;

constexpr std::chrono::seconds kDefaultRequestTimeout{30};

[[nodiscard]] std::string url_encode(std::string_view in) {
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
                                || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

[[nodiscard]] std::string make_form(
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    std::string out;
    bool first = true;
    for (const auto& [k, v] : fields) {
        if (!first)
            out.push_back('&');
        first = false;
        out.append(url_encode(k));
        out.push_back('=');
        out.append(url_encode(v));
    }
    return out;
}

/// Extracts `SID=<value>` from a `Set-Cookie` header value, or returns empty.
[[nodiscard]] std::string parse_sid(std::string_view set_cookie) {
    const auto pos = set_cookie.find("SID=");
    if (pos == std::string_view::npos)
        return {};
    auto rest = set_cookie.substr(pos + 4);
    const auto end = rest.find(';');
    return std::string{rest.substr(0, end)};
}

[[nodiscard]] std::string random_boundary() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);
    std::string b = "----CmlbQbBoundary";
    for (int i = 0; i < 16; ++i)
        b.push_back(alphabet[dist(rng)]);
    return b;
}

[[nodiscard]] DownloadState parse_state(std::string_view text) {
    if (text == "downloading" || text == "metaDL" || text == "stalledDL" || text == "checkingDL"
        || text == "forcedDL") {
        return DownloadState::Downloading;
    }
    if (text == "queuedDL" || text == "queuedUP" || text == "allocating") {
        return DownloadState::Queued;
    }
    if (text == "pausedDL" || text == "pausedUP") {
        return DownloadState::Paused;
    }
    if (text == "uploading" || text == "stalledUP" || text == "forcedUP" || text == "checkingUP") {
        return DownloadState::Seeding;
    }
    if (text == "error" || text == "missingFiles" || text == "unknown") {
        return DownloadState::Error;
    }
    if (text == "moving") {
        return DownloadState::Downloading;
    }
    // Unknown state value — qBit added something we haven't mapped. Log once
    // per distinct string so the operator notices without flooding the log
    // on a long-running torrent that keeps reporting the same value.
    static std::mutex seen_mu;
    static std::unordered_set<std::string, std::hash<std::string>, std::equal_to<>> seen;
    {
        std::lock_guard lk{seen_mu};
        const std::string key{text};
        if (seen.insert(key).second) {
            cmlb::core::Logger::warn(
                "qBittorrent: unknown torrent state '{}' — defaulting to Queued. "
                "Update parse_state() in qbittorrent_downloader.cpp.",
                key);
        }
    }
    return DownloadState::Queued;
}

[[nodiscard]] DownloadStatus to_status(const json& j) {
    DownloadStatus s;
    if (j.contains("hash") && j["hash"].is_string()) {
        s.id = Gid{j["hash"].get<std::string>()};
    }
    if (j.contains("name") && j["name"].is_string()) {
        s.name = j["name"].get<std::string>();
    }
    if (j.contains("state") && j["state"].is_string()) {
        s.state = parse_state(j["state"].get<std::string>());
    }

    s.total_bytes = j.value("size", std::int64_t{0});
    s.downloaded_bytes = j.value("completed", std::int64_t{0});
    s.uploaded_bytes = j.value("uploaded", std::int64_t{0});
    s.download_speed_bps = j.value("dlspeed", std::int64_t{0});
    s.upload_speed_bps = j.value("upspeed", std::int64_t{0});
    s.eta = std::chrono::seconds{j.value("eta", std::int64_t{0})};

    if (j.contains("save_path") && j["save_path"].is_string()) {
        s.save_path = std::filesystem::path{j["save_path"].get<std::string>()};
    }
    if (j.contains("num_seeds")) {
        s.num_seeders = j["num_seeds"].get<int>();
    }
    if (j.contains("num_leechs")) {
        s.num_leechers = j["num_leechs"].get<int>();
    }
    if (j.contains("ratio") && j["ratio"].is_number()) {
        s.seed_ratio = j["ratio"].get<float>();
    }
    return s;
}

} // namespace

/// Multipart file part — defined here so it can appear in `Impl::add_common`.
struct MultipartFile;

// ===========================================================================
// QbittorrentDownloader::Impl
// ===========================================================================
class QbittorrentDownloader::Impl {
public:
    Impl(cmlb::core::Executor& exec,
         cmlb::core::QbittorrentConfig config,
         http::BeastHttpClient& http_client)
        : executor_{exec}, config_{std::move(config)}, http_{http_client} {
        // Strip trailing slash so we can compose paths cleanly.
        while (!config_.url.empty() && config_.url.back() == '/') {
            config_.url.pop_back();
        }
    }

    [[nodiscard]] cmlb::core::Executor& executor() noexcept {
        return executor_;
    }

    [[nodiscard]] http::BeastHttpClient& http() noexcept {
        return http_;
    }

    [[nodiscard]] const cmlb::core::QbittorrentConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string session_id() {
        std::scoped_lock lock{mutex_};
        return session_id_;
    }

    void store_session(std::string sid) {
        std::scoped_lock lock{mutex_};
        session_id_ = std::move(sid);
        connected_.store(!session_id_.empty(), std::memory_order_release);
    }

    void clear_session() {
        std::scoped_lock lock{mutex_};
        session_id_.clear();
        connected_.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::string url(std::string_view path) const {
        std::string out = config_.url;
        if (!path.empty() && path.front() != '/')
            out.push_back('/');
        out.append(path);
        return out;
    }

    // Forward declarations for helpers used inside this Impl (defined later).
    boost::asio::awaitable<Result<std::unordered_set<std::string>>> snapshot_hashes();

    boost::asio::awaitable<Result<Gid>> add_common(
        std::vector<std::pair<std::string, std::string>> text_fields,
        std::vector<MultipartFile> files,
        const DownloadOptions& options);

    boost::asio::awaitable<Result<void>> ensure_login() {
        if (connected())
            co_return Result<void>{};
        co_return co_await login();
    }

    boost::asio::awaitable<Result<void>> login() {
        const std::string body =
            make_form({{"username", config_.username}, {"password", config_.password}});

        http::HttpRequest req;
        req.method = http::HttpMethod::Post;
        req.url = url("/api/v2/auth/login");
        req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        req.headers.push_back({"Referer", config_.url});
        req.body = body;
        req.timeout = kDefaultRequestTimeout;

        auto resp = co_await http_.request(req);
        if (!resp)
            co_return std::unexpected(resp.error());

        if (resp->status_code != 200) {
            co_return error(ErrorCode::Unauthenticated,
                            "qBittorrent login HTTP " + std::to_string(resp->status_code));
        }
        // qBit returns 200 + body "Fails." on bad creds.
        if (resp->body.find("Ok.") == std::string::npos) {
            co_return error(ErrorCode::Unauthenticated,
                            "qBittorrent login rejected: " + resp->body);
        }
        for (const auto& h : resp->headers) {
            if (h.name == "Set-Cookie" || h.name == "set-cookie") {
                if (auto sid = parse_sid(h.value); !sid.empty()) {
                    store_session(std::move(sid));
                    // Fire-and-soft-fail: push the throughput preferences.
                    // qBit silently ignores unknown keys on older builds, so
                    // a failure here is logged at warn but does not fail the
                    // login itself — the bot is still usable.
                    auto pref_res = co_await push_preferences();
                    if (!pref_res) {
                        cmlb::core::Logger::warn(
                            "qBittorrent: setPreferences failed (login still OK): {}",
                            pref_res.error().message);
                    } else {
                        cmlb::core::Logger::info("qBittorrent: setPreferences accepted "
                                                 "(transfers={}/{}, conns={}, async_io={}, "
                                                 "ratio={:.2f}, seed_time={}m)",
                                                 config_.max_active_downloads,
                                                 config_.max_active_uploads,
                                                 config_.max_connections,
                                                 config_.async_io_threads,
                                                 config_.seed_ratio_limit,
                                                 config_.seed_time_limit.count());
                    }
                    co_return Result<void>{};
                }
            }
        }
        co_return error(ErrorCode::Unauthenticated, "qBittorrent login: SID cookie missing");
    }

    /// Pushes the throughput-related fields of `QbittorrentConfig` to qBit's
    /// runtime preferences. Re-applied on every login because some qBit
    /// configurations don't persist these across daemon restarts.
    boost::asio::awaitable<Result<void>> push_preferences() {
        json prefs;
        prefs["max_active_downloads"] = config_.max_active_downloads;
        prefs["max_active_uploads"] = config_.max_active_uploads;
        prefs["max_active_torrents"] = config_.max_active_torrents;
        prefs["max_connec"] = config_.max_connections;
        prefs["max_connec_per_torrent"] = config_.max_connections_per_torrent;
        prefs["max_uploads"] = config_.max_uploads;
        prefs["max_uploads_per_torrent"] = config_.max_uploads_per_torrent;
        prefs["up_limit"] = config_.up_limit;
        prefs["dl_limit"] = config_.dl_limit;
        prefs["dht"] = config_.dht;
        prefs["pex"] = config_.pex;
        prefs["lsd"] = config_.lsd;
        prefs["anonymous_mode"] = config_.anonymous_mode;
        prefs["async_io_threads"] = config_.async_io_threads;
        prefs["disk_cache"] = config_.disk_cache_mib;
        // Seed-ratio / seed-time enforcement. qBit's runtime preferences map:
        //   max_ratio_enabled / max_ratio        — global ratio cap
        //   max_seeding_time_enabled / max_seeding_time (minutes)
        //   max_ratio_act — 0 = pause, 1 = remove (we pick "pause" so the
        //   torrent can be resumed manually if the user wants more sharing).
        if (config_.seed_ratio_limit > 0.0) {
            prefs["max_ratio_enabled"] = true;
            prefs["max_ratio"] = config_.seed_ratio_limit;
        } else {
            prefs["max_ratio_enabled"] = false;
        }
        if (config_.seed_time_limit.count() > 0) {
            prefs["max_seeding_time_enabled"] = true;
            prefs["max_seeding_time"] = static_cast<int>(config_.seed_time_limit.count());
        } else {
            prefs["max_seeding_time_enabled"] = false;
        }
        prefs["max_ratio_act"] = 0;

        // /api/v2/app/setPreferences accepts a form with `json=<urlencoded>`.
        const std::string body = "json=" + url_encode(prefs.dump());

        http::HttpRequest req;
        req.method = http::HttpMethod::Post;
        req.url = url("/api/v2/app/setPreferences");
        req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        req.headers.push_back({"Referer", config_.url});
        req.headers.push_back({"Cookie", "SID=" + session_id()});
        req.body = body;
        req.timeout = kDefaultRequestTimeout;

        auto resp = co_await http_.request(req);
        if (!resp)
            co_return std::unexpected(resp.error());
        if (resp->status_code != 200) {
            co_return error(ErrorCode::QbittorrentApi,
                            "setPreferences HTTP " + std::to_string(resp->status_code) + ": "
                                + resp->body);
        }
        co_return Result<void>{};
    }

    /// Issues `req` with the cookie attached. On 403, transparently
    /// re-authenticates and retries once.
    boost::asio::awaitable<Result<http::HttpResponse>> authed_request(http::HttpRequest req) {
        if (auto login_res = co_await ensure_login(); !login_res) {
            co_return std::unexpected(login_res.error());
        }
        req.headers.push_back({"Cookie", "SID=" + session_id()});

        auto resp = co_await http_.request(req);
        if (!resp)
            co_return std::unexpected(resp.error());

        if (resp->status_code == 403) {
            clear_session();
            // Remove the stale Cookie header before retrying.
            std::erase_if(req.headers, [](const http::HttpHeader& h) {
                return h.name == "Cookie";
            });
            if (auto login_res = co_await login(); !login_res) {
                co_return std::unexpected(login_res.error());
            }
            req.headers.push_back({"Cookie", "SID=" + session_id()});
            resp = co_await http_.request(req);
            if (!resp)
                co_return std::unexpected(resp.error());
        }
        co_return std::move(*resp);
    }

private:
    cmlb::core::Executor& executor_;
    cmlb::core::QbittorrentConfig config_;
    http::BeastHttpClient& http_;

    std::mutex mutex_;
    std::string session_id_;
    std::atomic<bool> connected_{false};
};

// ===========================================================================
// Construction
// ===========================================================================

QbittorrentDownloader::QbittorrentDownloader(cmlb::core::Executor& executor,
                                             cmlb::core::QbittorrentConfig config,
                                             http::BeastHttpClient& http_client)
    : impl_{std::make_unique<Impl>(executor, std::move(config), http_client)} {
}

QbittorrentDownloader::~QbittorrentDownloader() = default;

bool QbittorrentDownloader::is_connected() const noexcept {
    return impl_ && impl_->connected();
}

std::string_view QbittorrentDownloader::client_name() const noexcept {
    return "qbittorrent";
}

// ===========================================================================
// Helpers
// ===========================================================================

/// Builds a multipart/form-data body from a list of text fields plus an
/// optional file part `(field_name, filename, content_type, bytes)`.
struct MultipartFile {
    std::string field_name;
    std::string file_name;
    std::string content_type;
    std::span<const std::byte> bytes;
};

namespace {

[[nodiscard]] std::string build_multipart(
    std::string_view boundary,
    const std::vector<std::pair<std::string, std::string>>& text_fields,
    const std::vector<MultipartFile>& files) {
    std::string out;
    for (const auto& [name, value] : text_fields) {
        out.append("--").append(boundary).append("\r\n");
        out.append("Content-Disposition: form-data; name=\"").append(name).append("\"\r\n\r\n");
        out.append(value).append("\r\n");
    }
    for (const auto& f : files) {
        out.append("--").append(boundary).append("\r\n");
        out.append("Content-Disposition: form-data; name=\"")
            .append(f.field_name)
            .append("\"; filename=\"")
            .append(f.file_name)
            .append("\"\r\n");
        out.append("Content-Type: ").append(f.content_type).append("\r\n\r\n");
        out.append(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size());
        out.append("\r\n");
    }
    out.append("--").append(boundary).append("--\r\n");
    return out;
}

} // namespace

boost::asio::awaitable<Result<std::unordered_set<std::string>>>
QbittorrentDownloader::Impl::snapshot_hashes() {
    http::HttpRequest req;
    req.method = http::HttpMethod::Get;
    req.url = url("/api/v2/torrents/info");
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/info HTTP " + std::to_string(resp->status_code));
    }
    std::unordered_set<std::string> out;
    try {
        auto arr = json::parse(resp->body);
        if (arr.is_array()) {
            out.reserve(arr.size());
            for (const auto& t : arr) {
                if (t.contains("hash") && t["hash"].is_string()) {
                    out.insert(t["hash"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& ex) {
        co_return error(ErrorCode::JsonParse, ex.what());
    }
    co_return out;
}

boost::asio::awaitable<Result<Gid>> QbittorrentDownloader::Impl::add_common(
    std::vector<std::pair<std::string, std::string>> text_fields,
    std::vector<MultipartFile> files,
    const DownloadOptions& options) {
    if (options.save_directory.has_value()) {
        text_fields.emplace_back("savepath", options.save_directory->generic_string());
    }
    for (const auto& [k, v] : options.extras) {
        text_fields.emplace_back(k, v);
    }

    auto before_res = co_await snapshot_hashes();
    if (!before_res)
        co_return std::unexpected(before_res.error());

    const std::string boundary = random_boundary();
    http::HttpRequest req;
    req.method = http::HttpMethod::Post;
    req.url = url("/api/v2/torrents/add");
    req.headers.push_back({"Content-Type", "multipart/form-data; boundary=" + boundary});
    req.body = build_multipart(boundary, text_fields, files);
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/add HTTP " + std::to_string(resp->status_code) + ": "
                            + resp->body);
    }
    if (resp->body.find("Fails") != std::string::npos) {
        co_return error(ErrorCode::QbittorrentApi, "torrents/add rejected: " + resp->body);
    }

    auto after_res = co_await snapshot_hashes();
    if (!after_res)
        co_return std::unexpected(after_res.error());
    for (const auto& h : *after_res) {
        if (!before_res->contains(h)) {
            co_return Gid{h};
        }
    }
    co_return error(ErrorCode::QbittorrentApi,
                    "torrents/add accepted but no new hash visible (duplicate?)");
}

// ===========================================================================
// Operations
// ===========================================================================

boost::asio::awaitable<Result<Gid>> QbittorrentDownloader::add_uri(std::string_view uri,
                                                                   DownloadOptions options) {
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("urls", std::string{uri});
    co_return co_await impl_->add_common(std::move(fields), {}, options);
}

boost::asio::awaitable<Result<Gid>> QbittorrentDownloader::add_torrent(
    std::span<const std::byte> torrent_data, DownloadOptions options) {
    std::vector<MultipartFile> files;
    files.push_back({"torrents", "upload.torrent", "application/x-bittorrent", torrent_data});
    co_return co_await impl_->add_common({}, std::move(files), options);
}

boost::asio::awaitable<Result<void>> QbittorrentDownloader::pause(Gid id) {
    http::HttpRequest req;
    req.method = http::HttpMethod::Post;
    req.url = impl_->url("/api/v2/torrents/pause");
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body = "hashes=" + url_encode(id.value());
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/pause HTTP " + std::to_string(resp->status_code));
    }
    co_return Result<void>{};
}

boost::asio::awaitable<Result<void>> QbittorrentDownloader::resume(Gid id) {
    http::HttpRequest req;
    req.method = http::HttpMethod::Post;
    req.url = impl_->url("/api/v2/torrents/resume");
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body = "hashes=" + url_encode(id.value());
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/resume HTTP " + std::to_string(resp->status_code));
    }
    co_return Result<void>{};
}

boost::asio::awaitable<Result<void>> QbittorrentDownloader::remove(Gid id, bool delete_files) {
    http::HttpRequest req;
    req.method = http::HttpMethod::Post;
    req.url = impl_->url("/api/v2/torrents/delete");
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body =
        make_form({{"hashes", id.value()}, {"deleteFiles", delete_files ? "true" : "false"}});
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/delete HTTP " + std::to_string(resp->status_code));
    }
    co_return Result<void>{};
}

boost::asio::awaitable<Result<DownloadStatus>> QbittorrentDownloader::status(Gid id) {
    http::HttpRequest req;
    req.method = http::HttpMethod::Get;
    req.url = impl_->url("/api/v2/torrents/info?hashes=" + url_encode(id.value()));
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/info HTTP " + std::to_string(resp->status_code));
    }
    try {
        auto arr = json::parse(resp->body);
        if (!arr.is_array() || arr.empty()) {
            co_return error(ErrorCode::NotFound, "qBittorrent torrent not found: " + id.value());
        }
        co_return to_status(arr.front());
    } catch (const std::exception& ex) {
        co_return error(ErrorCode::JsonParse, ex.what());
    }
}

boost::asio::awaitable<Result<std::vector<DownloadStatus>>> QbittorrentDownloader::active() {
    http::HttpRequest req;
    req.method = http::HttpMethod::Get;
    req.url = impl_->url("/api/v2/torrents/info?filter=downloading");
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "torrents/info HTTP " + std::to_string(resp->status_code));
    }
    try {
        auto arr = json::parse(resp->body);
        std::vector<DownloadStatus> out;
        if (arr.is_array()) {
            out.reserve(arr.size());
            for (const auto& t : arr) {
                out.push_back(to_status(t));
            }
        }
        co_return out;
    } catch (const std::exception& ex) {
        co_return error(ErrorCode::JsonParse, ex.what());
    }
}

boost::asio::awaitable<Result<GlobalStats>> QbittorrentDownloader::global_stats() {
    http::HttpRequest req;
    req.method = http::HttpMethod::Get;
    req.url = impl_->url("/api/v2/transfer/info");
    req.timeout = kDefaultRequestTimeout;

    auto resp = co_await impl_->authed_request(req);
    if (!resp)
        co_return std::unexpected(resp.error());
    if (resp->status_code != 200) {
        co_return error(ErrorCode::QbittorrentApi,
                        "transfer/info HTTP " + std::to_string(resp->status_code));
    }
    try {
        auto j = json::parse(resp->body);
        GlobalStats stats;
        stats.download_speed_bps = j.value("dl_info_speed", std::int64_t{0});
        stats.upload_speed_bps = j.value("up_info_speed", std::int64_t{0});

        // qBit doesn't surface queue counts on /transfer/info; derive from /info.
        http::HttpRequest infoReq;
        infoReq.method = http::HttpMethod::Get;
        infoReq.url = impl_->url("/api/v2/torrents/info");
        infoReq.timeout = kDefaultRequestTimeout;
        auto info_resp = co_await impl_->authed_request(infoReq);
        if (info_resp && info_resp->status_code == 200) {
            try {
                auto arr = json::parse(info_resp->body);
                if (arr.is_array()) {
                    for (const auto& t : arr) {
                        const auto state = t.value("state", std::string{});
                        const auto ds = parse_state(state);
                        switch (ds) {
                        case DownloadState::Downloading:
                            ++stats.active_count;
                            break;
                        case DownloadState::Queued:
                            ++stats.waiting_count;
                            break;
                        default:
                            ++stats.stopped_count;
                            break;
                        }
                    }
                }
            } catch (const std::exception&) {
                // Best effort — don't fail global_stats over a count breakdown.
            }
        }
        co_return stats;
    } catch (const std::exception& ex) {
        co_return error(ErrorCode::JsonParse, ex.what());
    }
}

} // namespace cmlb::infrastructure::download
