// ---------------------------------------------------------------------------
// src/infrastructure/download/aria2_downloader.cpp
//
// aria2 JSON-RPC over WebSocket. Single persistent ws (or wss) connection,
// async request/response correlation via per-request promise-like slots,
// exponential-backoff reconnect on disconnect.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/system/error_code.hpp>

#include <nlohmann/json.hpp>

#include <cmlb/core/cancellation.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/download/aria2_downloader.hpp>

namespace cmlb::infrastructure::download {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using cmlb::core::AppError;
using cmlb::core::error;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;
using cmlb::domain::Gid;

constexpr std::chrono::seconds kReconnectInitial{1};
constexpr std::chrono::seconds kReconnectMax{30};

/// Global runtime options sent via `aria2.changeGlobalOption` once the
/// WebSocket session is established. Declared at the top of the anonymous
/// namespace so the reconnect-time tuning coroutine (inside Impl::run_loop)
/// can see it. Re-declared as `inline` so the unused-function warning is
/// silenced on builds where the throughput-push code path is excluded
/// (e.g. via future #ifdef).
[[nodiscard]] inline nlohmann::json build_global_throughput_options() {
    nlohmann::json out = nlohmann::json::object();
    out["max-connection-per-server"] = "16";
    out["split"] = "16";
    out["min-split-size"] = "1M";
    out["disk-cache"] = "128M";
    out["max-tries"] = "5";
    out["retry-wait"] = "5";
    out["max-overall-download-limit"] = "0"; // unlimited
    out["max-overall-upload-limit"] = "0";   // unlimited (seeding)
    out["max-file-not-found"] = "10";
    out["max-concurrent-downloads"] = "16";
    return out;
}

/// Parses the scheme/host/port/target out of a `ws://host:port/path` or
/// `wss://host:port/path` URL. Returns `nullopt` on malformed input.
struct ParsedWsUrl {
    bool tls{false};
    std::string host;
    std::string port;
    std::string target;
};

[[nodiscard]] std::optional<ParsedWsUrl> parse_ws_url(std::string_view url) {
    ParsedWsUrl out;
    std::string_view rest;
    if (url.starts_with("wss://")) {
        out.tls = true;
        rest = url.substr(6);
    } else if (url.starts_with("ws://")) {
        out.tls = false;
        rest = url.substr(5);
    } else {
        return std::nullopt;
    }

    const auto slash = rest.find('/');
    const std::string_view authority =
        (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
    out.target =
        (slash == std::string_view::npos) ? std::string{"/"} : std::string{rest.substr(slash)};

    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        out.host = std::string{authority};
        out.port = out.tls ? "443" : "80";
    } else {
        out.host = std::string{authority.substr(0, colon)};
        out.port = std::string{authority.substr(colon + 1)};
    }
    if (out.host.empty() || out.port.empty()) {
        return std::nullopt;
    }
    return out;
}

/// Generates a short alphanumeric request id (12 chars). aria2 echoes this
/// back unchanged in the response's `id` field.
[[nodiscard]] std::string make_request_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    out.reserve(12);
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);
    for (int i = 0; i < 12; ++i) {
        out.push_back(alphabet[dist(rng)]);
    }
    return out;
}

/// Base64-encodes a binary buffer. aria2 requires this for `addTorrent`.
[[nodiscard]] std::string base64_encode(std::span<const std::byte> data) {
    static constexpr std::string_view tab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const auto b0 = static_cast<unsigned>(data[i]);
        const auto b1 = static_cast<unsigned>(data[i + 1]);
        const auto b2 = static_cast<unsigned>(data[i + 2]);
        out.push_back(tab[(b0 >> 2) & 0x3F]);
        out.push_back(tab[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(tab[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        out.push_back(tab[b2 & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        const auto b0 = static_cast<unsigned>(data[i]);
        const auto b1 = (i + 1 < data.size()) ? static_cast<unsigned>(data[i + 1]) : 0U;
        out.push_back(tab[(b0 >> 2) & 0x3F]);
        out.push_back(tab[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        if (i + 1 < data.size()) {
            out.push_back(tab[(b1 << 2) & 0x3F]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

/// Maps an aria2 textual status to our normalized enum.
[[nodiscard]] DownloadState parse_state(std::string_view text) noexcept {
    if (text == "active")
        return DownloadState::Downloading;
    if (text == "waiting")
        return DownloadState::Queued;
    if (text == "paused")
        return DownloadState::Paused;
    if (text == "complete")
        return DownloadState::Complete;
    if (text == "error")
        return DownloadState::Error;
    if (text == "removed")
        return DownloadState::Removed;
    return DownloadState::Queued;
}

/// Coerces an aria2 numeric field (returned as a JSON string!) to int64.
[[nodiscard]] std::int64_t json_str_int64(const json& v, std::int64_t fallback = 0) {
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    if (v.is_number_integer()) {
        return v.get<std::int64_t>();
    }
    return fallback;
}

/// Translates an aria2 `tellStatus` payload into our `DownloadStatus`.
[[nodiscard]] DownloadStatus to_status(const json& j) {
    DownloadStatus s;
    if (j.contains("gid") && j["gid"].is_string()) {
        s.id = Gid{j["gid"].get<std::string>()};
    }
    if (j.contains("status") && j["status"].is_string()) {
        s.state = parse_state(j["status"].get<std::string>());
    }
    s.total_bytes = json_str_int64(j.value("totalLength", json{}));
    s.downloaded_bytes = json_str_int64(j.value("completedLength", json{}));
    s.uploaded_bytes = json_str_int64(j.value("uploadLength", json{}));
    s.download_speed_bps = json_str_int64(j.value("downloadSpeed", json{}));
    s.upload_speed_bps = json_str_int64(j.value("uploadSpeed", json{}));

    if (s.download_speed_bps > 0 && s.total_bytes > 0) {
        const auto remaining = s.total_bytes - s.downloaded_bytes;
        s.eta = std::chrono::seconds{remaining > 0 ? remaining / s.download_speed_bps : 0};
    }

    if (j.contains("errorMessage") && j["errorMessage"].is_string()) {
        s.error_message = j["errorMessage"].get<std::string>();
    }
    if (j.contains("dir") && j["dir"].is_string()) {
        s.save_path = std::filesystem::path{j["dir"].get<std::string>()};
    }
    if (j.contains("files") && j["files"].is_array()) {
        for (const auto& f : j["files"]) {
            if (f.contains("path") && f["path"].is_string()) {
                s.files.emplace_back(f["path"].get<std::string>());
            }
        }
        if (!s.files.empty() && s.name.empty()) {
            s.name = s.files.front().filename().string();
        }
    }
    if (j.contains("bittorrent") && j["bittorrent"].is_object()) {
        const auto& bt = j["bittorrent"];
        if (bt.contains("info") && bt["info"].is_object() && bt["info"].contains("name")
            && bt["info"]["name"].is_string()) {
            s.name = bt["info"]["name"].get<std::string>();
        }
        if (j.contains("numSeeders")) {
            s.num_seeders = static_cast<int>(json_str_int64(j["numSeeders"]));
        }
        if (j.contains("connections")) {
            s.num_leechers = static_cast<int>(json_str_int64(j["connections"]));
        }
        if (s.uploaded_bytes > 0 && s.downloaded_bytes > 0) {
            s.seed_ratio = static_cast<float>(static_cast<double>(s.uploaded_bytes)
                                              / static_cast<double>(s.downloaded_bytes));
        }
    }
    return s;
}

} // namespace

// ===========================================================================
// Aria2Downloader::Impl
// ===========================================================================
class Aria2Downloader::Impl : public std::enable_shared_from_this<Aria2Downloader::Impl> {
public:
    Impl(cmlb::core::Executor& exec, cmlb::core::Aria2Config config)
        : executor_{exec},
          strand_{asio::make_strand(exec.get_executor())},
          ssl_ctx_{asio::ssl::context::tls_client},
          reconnect_timer_{strand_},
          config_{std::move(config)} {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(asio::ssl::verify_peer);
    }

    ~Impl() {
        // By the time we get here every coroutine that captured a
        // shared_from_this() must have already released its reference —
        // otherwise the shared_ptr would still keep us alive. So the streams
        // and timers are no longer in use; we just unwind defensively.
        close_stream_blocking();
    }

    /// Signals the background read/reconnect loop to exit at its next
    /// suspension point. Safe to call from any thread.
    void request_shutdown() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
        boost::system::error_code ec;
        // Cancel the reconnect timer and any in-flight read by closing the
        // underlying socket. The reader will surface this as an error and
        // bail out of the loop, dropping its captured `self`.
        try {
            asio::post(strand_, [self = weak_from_this()] {
                if (auto s = self.lock()) {
                    s->reconnect_timer_.cancel();
                    s->close_stream_async();
                }
            });
        } catch (...) {
            // best-effort
        }
    }

    void start() {
        // Kick off the reader/connection coroutine on the strand.
        auto self = shared_from_this();
        asio::co_spawn(
            strand_,
            [self]() -> asio::awaitable<void> {
                co_await self->run_loop();
            },
            asio::detached);
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------
    // Public RPC helpers (all suspend until the response arrives).
    // -------------------------------------------------------------------

    asio::awaitable<Result<json>> call(std::string method, json params) {
        // Prepend the secret token to the params array if configured.
        if (!config_.secret.empty()) {
            json with_token = json::array();
            with_token.push_back("token:" + config_.secret);
            for (auto& p : params) {
                with_token.push_back(std::move(p));
            }
            params = std::move(with_token);
        }

        const std::string id = make_request_id();
        json req = {
            {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};

        auto op = call_with_correlation(id, std::move(req));
        co_return co_await cmlb::core::with_timeout<json>(
            std::move(op),
            std::chrono::duration_cast<std::chrono::milliseconds>(config_.request_timeout));
    }

    [[nodiscard]] cmlb::core::Executor& executor() noexcept {
        return executor_;
    }

private:
    using WsPlain = ws::stream<beast::tcp_stream>;
    using WsTls = ws::stream<asio::ssl::stream<beast::tcp_stream>>;

    struct PendingCall {
        // Filled by the reader coroutine.
        std::optional<json> response;
        std::optional<AppError> failure;
        // Triggered to wake the awaiting call().
        std::shared_ptr<asio::steady_timer> wake;
    };

    asio::awaitable<Result<json>> call_with_correlation(std::string id, json req) {
        // Slot setup must happen on the strand to avoid racing with the reader.
        // The wake timer also lives on the strand so cross-thread cancel() is
        // serialized through the strand.
        co_await asio::post(asio::bind_executor(strand_, asio::use_awaitable));

        auto pending = std::make_shared<PendingCall>();
        pending->wake = std::make_shared<asio::steady_timer>(strand_);
        pending->wake->expires_at(std::chrono::steady_clock::time_point::max());
        pending_.emplace(id, pending);

        // Ensure we have a live connection. ensure_connected_() returns true
        // when the stream is ready, false when we should bail with Network.
        if (!connected_.load(std::memory_order_acquire)) {
            pending_.erase(id);
            co_return error(ErrorCode::Network, "aria2 WebSocket not connected");
        }

        // Serialize the JSON request and send it.
        const std::string payload = req.dump();
        auto send_res = co_await send_text(payload);
        if (!send_res) {
            pending_.erase(id);
            co_return std::unexpected(send_res.error());
        }

        // Suspend until the reader wakes us. The timer is cancelled (not
        // expired) when the response is delivered, which surfaces as
        // operation_aborted — that is success, not failure.
        boost::system::error_code wait_ec;
        co_await pending->wake->async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));

        // Pull the result off the slot under the strand.
        co_await asio::post(asio::bind_executor(strand_, asio::use_awaitable));
        pending_.erase(id);

        if (pending->response.has_value()) {
            co_return std::move(*pending->response);
        }
        if (pending->failure.has_value()) {
            co_return std::unexpected(std::move(*pending->failure));
        }
        co_return error(ErrorCode::Aria2Rpc, "aria2 RPC slot abandoned without response");
    }

    asio::awaitable<Result<void>> send_text(std::string payload) {
        // Must run on the strand (writes are not externally synchronized).
        co_await asio::post(asio::bind_executor(strand_, asio::use_awaitable));

        try {
            if (tls_stream_) {
                co_await tls_stream_->async_write(asio::buffer(payload), asio::use_awaitable);
            } else if (plain_stream_) {
                co_await plain_stream_->async_write(asio::buffer(payload), asio::use_awaitable);
            } else {
                co_return error(ErrorCode::Network, "aria2 WebSocket stream not initialized");
            }
        } catch (const boost::system::system_error& ex) {
            Logger::warn("aria2 write failed: {}", ex.what());
            fail_all_pending(error(ErrorCode::Network, ex.what()));
            close_stream_async();
            co_return error(ErrorCode::Network, ex.what());
        } catch (const std::exception& ex) {
            co_return error(ErrorCode::Network, ex.what());
        }
        co_return Result<void>{};
    }

    asio::awaitable<void> run_loop() {
        std::chrono::seconds backoff = kReconnectInitial;
        while (!shutdown_requested_.load(std::memory_order_acquire)) {
            auto connect_res = co_await connect_once();
            if (connect_res) {
                backoff = kReconnectInitial;
                // Push throughput defaults to the daemon. Fire-and-forget on
                // the same strand: the call suspends on the response, the
                // read loop (started below) routes the reply back.
                auto self_for_tuning = shared_from_this();
                asio::co_spawn(
                    strand_,
                    [self_for_tuning]() -> asio::awaitable<void> {
                        json params = json::array();
                        params.push_back(build_global_throughput_options());
                        auto r = co_await self_for_tuning->call("aria2.changeGlobalOption",
                                                                std::move(params));
                        if (!r.has_value()) {
                            Logger::warn("aria2 changeGlobalOption failed: {}", r.error().message);
                        } else {
                            Logger::info("aria2 throughput options applied "
                                         "(max-conn-per-server=16, split=16, "
                                         "disk-cache=128M)");
                        }
                    },
                    asio::detached);
                co_await read_until_disconnect();
            } else {
                Logger::warn("aria2 connect failed: {}", connect_res.error().message);
            }

            if (shutdown_requested_.load(std::memory_order_acquire)) {
                break;
            }

            // Backoff and reconnect.
            reconnect_timer_.expires_after(backoff);
            boost::system::error_code timer_ec;
            co_await reconnect_timer_.async_wait(
                asio::redirect_error(asio::use_awaitable, timer_ec));
            if (timer_ec == asio::error::operation_aborted) {
                break;
            }
            backoff = std::min(backoff * 2, kReconnectMax);
        }
        co_return;
    }

    asio::awaitable<Result<void>> connect_once() {
        auto parsed = parse_ws_url(config_.rpc_url);
        if (!parsed) {
            co_return error(ErrorCode::InvalidConfiguration,
                            "malformed aria2 rpc_url: " + config_.rpc_url);
        }

        try {
            tcp::resolver resolver{strand_};
            auto endpoints =
                co_await resolver.async_resolve(parsed->host, parsed->port, asio::use_awaitable);

            if (parsed->tls) {
                tls_stream_ = std::make_unique<WsTls>(strand_, ssl_ctx_);
                auto& lowest = beast::get_lowest_layer(*tls_stream_);
                lowest.expires_after(std::chrono::seconds{15});
                co_await lowest.async_connect(endpoints, asio::use_awaitable);

                // SNI is required by most TLS terminators. The OpenSSL macro
                // expands to an old-style cast to `void*`; suppress the
                // warning for this single call.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
                if (!SSL_set_tlsext_host_name(tls_stream_->next_layer().native_handle(),
                                              parsed->host.c_str())) {
#pragma GCC diagnostic pop
                    co_return error(ErrorCode::Network, "failed to set TLS SNI hostname");
                }
#pragma GCC diagnostic pop

                co_await tls_stream_->next_layer().async_handshake(asio::ssl::stream_base::client,
                                                                   asio::use_awaitable);
                beast::get_lowest_layer(*tls_stream_).expires_never();
                tls_stream_->set_option(
                    ws::stream_base::timeout::suggested(beast::role_type::client));
                co_await tls_stream_->async_handshake(
                    parsed->host + ":" + parsed->port, parsed->target, asio::use_awaitable);
            } else {
                plain_stream_ = std::make_unique<WsPlain>(strand_);
                auto& lowest = beast::get_lowest_layer(*plain_stream_);
                lowest.expires_after(std::chrono::seconds{15});
                co_await lowest.async_connect(endpoints, asio::use_awaitable);
                lowest.expires_never();
                plain_stream_->set_option(
                    ws::stream_base::timeout::suggested(beast::role_type::client));
                co_await plain_stream_->async_handshake(
                    parsed->host + ":" + parsed->port, parsed->target, asio::use_awaitable);
            }

            connected_.store(true, std::memory_order_release);
            Logger::info("aria2 connected to {}", config_.rpc_url);
            co_return Result<void>{};
        } catch (const std::exception& ex) {
            connected_.store(false, std::memory_order_release);
            tls_stream_.reset();
            plain_stream_.reset();
            co_return error(ErrorCode::Network, ex.what());
        }
    }

    asio::awaitable<void> read_until_disconnect() {
        beast::flat_buffer buffer;
        try {
            while (!shutdown_requested_.load(std::memory_order_acquire)) {
                buffer.clear();
                std::size_t n = 0;
                if (tls_stream_) {
                    n = co_await tls_stream_->async_read(buffer, asio::use_awaitable);
                } else if (plain_stream_) {
                    n = co_await plain_stream_->async_read(buffer, asio::use_awaitable);
                } else {
                    break;
                }
                (void)n;

                auto data = buffer.data();
                std::string payload{static_cast<const char*>(data.data()), data.size()};
                handle_message(std::move(payload));
            }
        } catch (const boost::system::system_error& ex) {
            Logger::info("aria2 read loop exit: {}", ex.what());
        } catch (const std::exception& ex) {
            Logger::warn("aria2 read loop exception: {}", ex.what());
        }
        connected_.store(false, std::memory_order_release);
        fail_all_pending(error(ErrorCode::Network, "aria2 connection lost"));
        plain_stream_.reset();
        tls_stream_.reset();
        co_return;
    }

    void handle_message(std::string text) {
        json doc;
        try {
            doc = json::parse(text);
        } catch (const std::exception& ex) {
            Logger::warn("aria2 sent malformed JSON: {}", ex.what());
            return;
        }

        // Notifications (e.g. aria2.onDownloadComplete) have no id — ignore.
        if (!doc.contains("id") || !doc["id"].is_string()) {
            return;
        }
        const std::string id = doc["id"].get<std::string>();
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }
        auto pending = it->second;

        if (doc.contains("error")) {
            const auto& e = doc["error"];
            const int code = e.value("code", -1);
            const std::string msg = "aria2 RPC error " + std::to_string(code) + ": "
                                    + e.value("message", std::string{"unknown"});
            pending->failure = AppError{ErrorCode::Aria2Rpc, msg};
        } else if (doc.contains("result")) {
            pending->response = doc["result"];
        } else {
            pending->failure =
                AppError{ErrorCode::Aria2Rpc, "aria2 response missing both result and error"};
        }
        pending->wake->cancel();
    }

    void fail_all_pending(std::unexpected<AppError> err) {
        for (auto& [id, slot] : pending_) {
            slot->failure = err.error();
            slot->wake->cancel();
        }
    }

    void close_stream_async() {
        // Modern Boost.Beast `tcp_stream::close()` no longer accepts an
        // error_code overload — call the no-arg version and let it cancel
        // any pending I/O via its standard cancellation mechanism.
        if (tls_stream_) {
            tls_stream_->next_layer().next_layer().close();
        }
        if (plain_stream_) {
            plain_stream_->next_layer().close();
        }
    }

    void close_stream_blocking() noexcept {
        try {
            if (tls_stream_) {
                tls_stream_->next_layer().next_layer().close();
            }
            if (plain_stream_) {
                plain_stream_->next_layer().close();
            }
        } catch (...) {
            // best effort
        }
    }

    cmlb::core::Executor& executor_;
    asio::strand<asio::any_io_executor> strand_;
    asio::ssl::context ssl_ctx_;
    asio::steady_timer reconnect_timer_;
    cmlb::core::Aria2Config config_;

    std::unique_ptr<WsPlain> plain_stream_;
    std::unique_ptr<WsTls> tls_stream_;

    std::unordered_map<std::string, std::shared_ptr<PendingCall>> pending_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> shutdown_requested_{false};
};

// ===========================================================================
// Aria2Downloader public API
// ===========================================================================

Aria2Downloader::Aria2Downloader(cmlb::core::Executor& executor, cmlb::core::Aria2Config config)
    : impl_{std::make_shared<Impl>(executor, std::move(config))} {
    impl_->start();
}

Aria2Downloader::~Aria2Downloader() {
    if (impl_) {
        impl_->request_shutdown();
    }
}

bool Aria2Downloader::is_connected() const noexcept {
    return impl_ && impl_->connected();
}

std::string_view Aria2Downloader::client_name() const noexcept {
    return "aria2";
}

namespace {

/// Per-task throughput defaults. Tuned for "use my whole pipe" — every
/// HTTP/FTP download fans out to 16 concurrent connections, torrents
/// preallocate space (avoids fragmentation on spinning disks), and aria2
/// keeps a fat in-memory cache so it does not stall on flush.
///
/// Callers can override any field by passing the same key in
/// `DownloadOptions::extras` (per-task overrides win since they are merged
/// after the defaults).
[[nodiscard]] json build_options(const DownloadOptions& opts) {
    json out = json::object();
    out["max-connection-per-server"] = "16";
    out["split"] = "16";
    out["min-split-size"] = "1M";
    out["max-tries"] = "5";
    out["retry-wait"] = "5";
    out["continue"] = "true";
    out["file-allocation"] = "falloc";
    out["piece-length"] = "1M";
    out["check-integrity"] = "false";
    out["allow-overwrite"] = "false";
    out["auto-file-renaming"] = "true";
    out["always-resume"] = "true";

    if (opts.save_directory.has_value()) {
        out["dir"] = opts.save_directory->generic_string();
    }
    for (const auto& [k, v] : opts.extras) {
        out[k] = v; // user/task-level override beats throughput defaults
    }
    return out;
}

} // namespace

asio::awaitable<Result<Gid>> Aria2Downloader::add_uri(std::string_view uri,
                                                      DownloadOptions options) {
    json params = json::array();
    params.push_back(json::array({std::string{uri}}));
    params.push_back(build_options(options));

    auto res = co_await impl_->call("aria2.addUri", std::move(params));
    if (!res) {
        co_return std::unexpected(res.error());
    }
    if (!res->is_string()) {
        co_return error(ErrorCode::Aria2Rpc, "aria2.addUri returned non-string gid");
    }
    co_return Gid{res->get<std::string>()};
}

asio::awaitable<Result<Gid>> Aria2Downloader::add_torrent(std::span<const std::byte> torrent_data,
                                                          DownloadOptions options) {
    json params = json::array();
    params.push_back(base64_encode(torrent_data));
    params.push_back(json::array()); // uris (empty)
    params.push_back(build_options(options));

    auto res = co_await impl_->call("aria2.addTorrent", std::move(params));
    if (!res) {
        co_return std::unexpected(res.error());
    }
    if (!res->is_string()) {
        co_return error(ErrorCode::Aria2Rpc, "aria2.addTorrent returned non-string gid");
    }
    co_return Gid{res->get<std::string>()};
}

asio::awaitable<Result<void>> Aria2Downloader::pause(Gid id) {
    json params = json::array();
    params.push_back(id.value());
    auto res = co_await impl_->call("aria2.pause", std::move(params));
    if (!res)
        co_return std::unexpected(res.error());
    co_return Result<void>{};
}

asio::awaitable<Result<void>> Aria2Downloader::resume(Gid id) {
    json params = json::array();
    params.push_back(id.value());
    auto res = co_await impl_->call("aria2.unpause", std::move(params));
    if (!res)
        co_return std::unexpected(res.error());
    co_return Result<void>{};
}

asio::awaitable<Result<void>> Aria2Downloader::remove(Gid id, bool delete_files) {
    std::vector<std::filesystem::path> files;

    if (delete_files) {
        // First pull the file list — once removed, aria2 forgets it.
        json status_params = json::array();
        status_params.push_back(id.value());
        status_params.push_back(json::array({"files"}));
        auto status_res = co_await impl_->call("aria2.tellStatus", std::move(status_params));
        if (status_res && status_res->is_object() && status_res->contains("files")
            && (*status_res)["files"].is_array()) {
            for (const auto& f : (*status_res)["files"]) {
                if (f.contains("path") && f["path"].is_string()) {
                    files.emplace_back(f["path"].get<std::string>());
                }
            }
        }
    }

    json params = json::array();
    params.push_back(id.value());
    auto res = co_await impl_->call("aria2.remove", std::move(params));
    if (!res) {
        // aria2 returns "GID_NOT_FOUND" if it's already gone — surface to caller.
        co_return std::unexpected(res.error());
    }

    if (delete_files) {
        for (const auto& path : files) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            if (ec) {
                Logger::warn("aria2: failed to unlink {}: {}", path.generic_string(), ec.message());
            }
        }
    }
    co_return Result<void>{};
}

asio::awaitable<Result<DownloadStatus>> Aria2Downloader::status(Gid id) {
    json params = json::array();
    params.push_back(id.value());
    auto res = co_await impl_->call("aria2.tellStatus", std::move(params));
    if (!res)
        co_return std::unexpected(res.error());
    if (!res->is_object()) {
        co_return error(ErrorCode::Aria2Rpc, "aria2.tellStatus returned non-object");
    }
    co_return to_status(*res);
}

asio::awaitable<Result<std::vector<DownloadStatus>>> Aria2Downloader::active() {
    auto res = co_await impl_->call("aria2.tellActive", json::array());
    if (!res)
        co_return std::unexpected(res.error());
    if (!res->is_array()) {
        co_return error(ErrorCode::Aria2Rpc, "aria2.tellActive returned non-array");
    }
    std::vector<DownloadStatus> out;
    out.reserve(res->size());
    for (const auto& item : *res) {
        out.push_back(to_status(item));
    }
    co_return out;
}

asio::awaitable<Result<GlobalStats>> Aria2Downloader::global_stats() {
    auto res = co_await impl_->call("aria2.getGlobalStat", json::array());
    if (!res)
        co_return std::unexpected(res.error());
    if (!res->is_object()) {
        co_return error(ErrorCode::Aria2Rpc, "aria2.getGlobalStat returned non-object");
    }
    GlobalStats stats;
    stats.download_speed_bps = json_str_int64(res->value("downloadSpeed", json{}));
    stats.upload_speed_bps = json_str_int64(res->value("uploadSpeed", json{}));
    stats.active_count = static_cast<int>(json_str_int64(res->value("numActive", json{})));
    stats.waiting_count = static_cast<int>(json_str_int64(res->value("numWaiting", json{})));
    stats.stopped_count = static_cast<int>(json_str_int64(res->value("numStopped", json{})));
    co_return stats;
}

} // namespace cmlb::infrastructure::download
