// SPDX-License-Identifier: MIT
//
// Real HTTP/1.1 client over Boost.Beast.
//
// Responsibilities:
//   * URL parsing for http:// and https://, with default-port inference.
//   * Async DNS resolve + TCP connect + (for HTTPS) OpenSSL handshake.
//   * Keep-alive connection pool keyed on (scheme,host,port). Pool caps the
//     number of distinct endpoints and the number of idle sockets per
//     endpoint; sockets idle longer than kIdleEviction are dropped.
//   * Per-request timeout via beast::tcp_stream::expires_after.
//   * Up to 5 redirect hops, with Authorization stripped on cross-origin.
//   * download_to_file streams via http::response_parser<http::file_body>
//     so the body is never fully buffered. on_progress is throttled to
//     >= kProgressMinInterval between fires.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/file.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace cmlb::infrastructure::http {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bh = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using cmlb::core::AppError;
using cmlb::core::error;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;

constexpr std::size_t kMaxRedirects = 5;
constexpr std::size_t kMaxEndpoints = 32;
constexpr std::size_t kMaxSocketsPerEndpoint = 8;
constexpr std::chrono::seconds kIdleEviction{60};
constexpr std::chrono::milliseconds kProgressMinInterval{250};
constexpr const char* kUserAgent = "cmlb/1.0 (+https://github.com/cmlb)";

// ---------------------------------------------------------------------------
// URL parsing — RFC-3986-lite. We only need http:// and https:// and we treat
// userinfo as a parse error (callers should use Authorization headers).
// ---------------------------------------------------------------------------

struct ParsedUrl {
    bool https{false};
    std::string host;
    std::string port;
    std::string target; // path + "?" + query; never empty (defaults to "/")
};

[[nodiscard]] std::optional<ParsedUrl> parse_url(std::string_view url) {
    ParsedUrl out;
    std::string_view rest;
    if (url.starts_with("https://")) {
        out.https = true;
        rest = url.substr(8);
    } else if (url.starts_with("http://")) {
        out.https = false;
        rest = url.substr(7);
    } else {
        return std::nullopt;
    }

    const auto slash = rest.find('/');
    std::string_view authority = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
    out.target =
        (slash == std::string_view::npos) ? std::string{"/"} : std::string{rest.substr(slash)};

    if (authority.find('@') != std::string_view::npos) {
        // userinfo not supported; ambiguous with credentials in URL.
        return std::nullopt;
    }

    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        out.host = std::string{authority.substr(0, colon)};
        out.port = std::string{authority.substr(colon + 1)};
        if (out.host.empty() || out.port.empty())
            return std::nullopt;
        for (char c : out.port) {
            if (c < '0' || c > '9')
                return std::nullopt;
        }
    } else {
        out.host = std::string{authority};
        out.port = out.https ? "443" : "80";
    }
    if (out.host.empty())
        return std::nullopt;
    return out;
}

[[nodiscard]] bh::verb to_verb(HttpMethod m) noexcept {
    switch (m) {
    case HttpMethod::Get:
        return bh::verb::get;
    case HttpMethod::Post:
        return bh::verb::post;
    case HttpMethod::Put:
        return bh::verb::put;
    case HttpMethod::Delete_:
        return bh::verb::delete_;
    case HttpMethod::Patch:
        return bh::verb::patch;
    case HttpMethod::Head:
        return bh::verb::head;
    }
    return bh::verb::get;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

[[nodiscard]] bool same_origin(const ParsedUrl& a, const ParsedUrl& b) noexcept {
    return a.https == b.https && iequals(a.host, b.host) && a.port == b.port;
}

[[nodiscard]] std::unexpected<AppError> translate_ec(const boost::system::error_code& ec,
                                                     std::string_view context) {
    // Beast's `tcp_stream` surfaces deadline-timer expiry as
    // `beast::error::timeout`; external cancellation (asio `cancellation_slot`
    // firing, or an explicit `stream.cancel()`) surfaces as
    // `operation_aborted`. Conflating them mis-labels user `/cancel` actions
    // as Timeout — which in turn makes the GDrive uploader's
    // 5xx/Timeout-class retry path treat cancellation as a transient failure.
    if (ec == beast::error::timeout) {
        return std::unexpected{
            AppError{ErrorCode::Timeout, std::string{context} + ": " + ec.message()}};
    }
    if (ec == asio::error::operation_aborted) {
        return std::unexpected{
            AppError{ErrorCode::Cancelled, std::string{context} + ": " + ec.message()}};
    }
    if (ec == asio::error::host_not_found || ec == asio::error::host_not_found_try_again
        || ec == asio::error::no_data || ec == asio::error::network_unreachable
        || ec == asio::error::host_unreachable || ec == asio::error::connection_refused
        || ec == asio::error::connection_reset || ec == asio::error::connection_aborted
        || ec == asio::error::not_connected || ec == asio::error::network_down
        || ec == asio::error::network_reset || ec == asio::error::address_in_use
        || ec == asio::error::eof) {
        return std::unexpected{
            AppError{ErrorCode::Network, std::string{context} + ": " + ec.message()}};
    }
    return std::unexpected{AppError{ErrorCode::Io, std::string{context} + ": " + ec.message()}};
}

// ---------------------------------------------------------------------------
// Connection key + pool. Lives in the unnamed namespace so the type is local
// to this TU; ConnectionPool is in the surrounding namespace so it can be
// referenced by the forward declaration in the header.
// ---------------------------------------------------------------------------

struct EndpointKey {
    bool https{false};
    std::string host;
    std::string port;

    [[nodiscard]] bool operator==(const EndpointKey& o) const noexcept {
        return https == o.https && host == o.host && port == o.port;
    }
};

struct EndpointKeyHash {
    [[nodiscard]] std::size_t operator()(const EndpointKey& k) const noexcept {
        std::size_t h = std::hash<bool>{}(k.https);
        h ^= std::hash<std::string>{}(k.host) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct PlainSocket {
    beast::tcp_stream stream;
    std::chrono::steady_clock::time_point last_used{};

    explicit PlainSocket(asio::any_io_executor ex) : stream{std::move(ex)} {
    }
};

struct TlsSocket {
    asio::ssl::stream<beast::tcp_stream> stream;
    std::chrono::steady_clock::time_point last_used{};

    TlsSocket(asio::any_io_executor ex, asio::ssl::context& ctx) : stream{std::move(ex), ctx} {
    }
};

} // namespace

// ---------------------------------------------------------------------------
// ConnectionPool — keeps a bounded number of idle keep-alive sockets per
// endpoint. The pool itself is mutex-guarded (acquire / release happen from
// the request coroutine, which may run on any thread of the executor).
// ---------------------------------------------------------------------------

class ConnectionPool {
public:
    explicit ConnectionPool(asio::any_io_executor exec) noexcept : exec_{std::move(exec)} {
    }

    [[nodiscard]] std::unique_ptr<PlainSocket> take_plain(const EndpointKey& key) {
        std::scoped_lock guard{mutex_};
        auto& q = plain_idle_[key];
        prune_locked(q);
        if (q.empty())
            return nullptr;
        auto c = std::move(q.back());
        q.pop_back();
        return c;
    }

    [[nodiscard]] std::unique_ptr<TlsSocket> take_tls(const EndpointKey& key) {
        std::scoped_lock guard{mutex_};
        auto& q = tls_idle_[key];
        prune_locked(q);
        if (q.empty())
            return nullptr;
        auto c = std::move(q.back());
        q.pop_back();
        return c;
    }

    void return_plain(const EndpointKey& key, std::unique_ptr<PlainSocket> sock) {
        if (!sock)
            return;
        std::scoped_lock guard{mutex_};
        if (plain_idle_.size() + tls_idle_.size() >= kMaxEndpoints && !plain_idle_.contains(key)) {
            return; // pool too crowded, drop the socket
        }
        auto& q = plain_idle_[key];
        if (q.size() >= kMaxSocketsPerEndpoint) {
            return; // endpoint too crowded, drop
        }
        sock->last_used = std::chrono::steady_clock::now();
        q.push_back(std::move(sock));
    }

    void return_tls(const EndpointKey& key, std::unique_ptr<TlsSocket> sock) {
        if (!sock)
            return;
        std::scoped_lock guard{mutex_};
        if (plain_idle_.size() + tls_idle_.size() >= kMaxEndpoints && !tls_idle_.contains(key)) {
            return;
        }
        auto& q = tls_idle_[key];
        if (q.size() >= kMaxSocketsPerEndpoint) {
            return;
        }
        sock->last_used = std::chrono::steady_clock::now();
        q.push_back(std::move(sock));
    }

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return exec_;
    }

private:
    template <typename Sock>
    static void prune_locked(std::deque<std::unique_ptr<Sock>>& q) noexcept {
        const auto cutoff = std::chrono::steady_clock::now() - kIdleEviction;
        while (!q.empty() && q.front()->last_used < cutoff) {
            q.pop_front();
        }
    }

    asio::any_io_executor exec_;
    std::mutex mutex_;
    std::unordered_map<EndpointKey, std::deque<std::unique_ptr<PlainSocket>>, EndpointKeyHash>
        plain_idle_;
    std::unordered_map<EndpointKey, std::deque<std::unique_ptr<TlsSocket>>, EndpointKeyHash>
        tls_idle_;
};

namespace {

// ---------------------------------------------------------------------------
// Header helpers — Beast's request<>/response<> use string-view-only ops, so
// we convert HttpHeader vectors to/from them by hand.
// ---------------------------------------------------------------------------

void apply_headers(bh::request<bh::string_body>& req,
                   const std::vector<HttpHeader>& headers,
                   const std::string& host_with_port,
                   bool has_body) {
    bool saw_user_agent = false;
    bool saw_accept = false;
    bool saw_connection = false;
    for (const auto& h : headers) {
        if (iequals(h.name, "host"))
            continue; // controlled below
        if (iequals(h.name, "content-length"))
            continue; // controlled below
        if (iequals(h.name, "user-agent"))
            saw_user_agent = true;
        if (iequals(h.name, "accept"))
            saw_accept = true;
        if (iequals(h.name, "connection"))
            saw_connection = true;
        req.set(h.name, h.value);
    }
    req.set(bh::field::host, host_with_port);
    if (!saw_user_agent)
        req.set(bh::field::user_agent, kUserAgent);
    if (!saw_accept)
        req.set(bh::field::accept, "*/*");
    if (!saw_connection)
        req.set(bh::field::connection, "keep-alive");
    if (has_body) {
        req.set(bh::field::content_length, std::to_string(req.body().size()));
    }
}

[[nodiscard]] std::vector<HttpHeader> extract_headers(const bh::response_header<>& resp) {
    std::vector<HttpHeader> out;
    out.reserve(16);
    for (const auto& f : resp) {
        out.push_back({std::string{f.name_string()}, std::string{f.value()}});
    }
    return out;
}

[[nodiscard]] std::optional<std::string> find_header(const bh::response_header<>& resp,
                                                     std::string_view name) {
    for (const auto& f : resp) {
        if (iequals(f.name_string(), name)) {
            return std::string{f.value()};
        }
    }
    return std::nullopt;
}

// Resolve a relative redirect target against the current request URL.
[[nodiscard]] std::optional<std::string> resolve_redirect(const ParsedUrl& current,
                                                          std::string_view location) {
    if (location.empty())
        return std::nullopt;
    if (location.starts_with("http://") || location.starts_with("https://")) {
        return std::string{location};
    }
    const char* scheme = current.https ? "https://" : "http://";
    std::string base = std::string{scheme} + current.host;
    if ((current.https && current.port != "443") || (!current.https && current.port != "80")) {
        base += ":" + current.port;
    }
    if (location.starts_with('/')) {
        return base + std::string{location};
    }
    // Relative-to-target. Strip the last path segment off current.target.
    std::string tgt = current.target;
    const auto slash = tgt.rfind('/');
    if (slash != std::string::npos)
        tgt.resize(slash + 1);
    else
        tgt = "/";
    return base + tgt + std::string{location};
}

// ---------------------------------------------------------------------------
// I/O primitives — connect, TLS handshake, write, read. Each maps a Beast
// error_code into our AppError taxonomy via translate_ec().
// ---------------------------------------------------------------------------

asio::awaitable<Result<std::unique_ptr<PlainSocket>>> open_plain(
    asio::any_io_executor exec, const EndpointKey& key, std::chrono::milliseconds timeout) {
    auto sock = std::make_unique<PlainSocket>(exec);
    boost::system::error_code ec;
    tcp::resolver resolver{exec};
    auto endpoints = co_await resolver.async_resolve(
        key.host, key.port, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "dns resolve " + key.host);

    sock->stream.expires_after(timeout);
    co_await sock->stream.async_connect(endpoints, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "tcp connect " + key.host);

    co_return sock;
}

asio::awaitable<Result<std::unique_ptr<TlsSocket>>> open_tls(asio::any_io_executor exec,
                                                             asio::ssl::context& ssl_ctx,
                                                             const EndpointKey& key,
                                                             std::chrono::milliseconds timeout) {
    auto sock = std::make_unique<TlsSocket>(exec, ssl_ctx);
    // OpenSSL's SSL_set_tlsext_host_name macro expands to a C-style cast.
    // Locally suppress -Wold-style-cast around it on GCC/Clang.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    const bool sni_ok =
        SSL_set_tlsext_host_name(sock->stream.native_handle(), key.host.c_str()) != 0;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (!sni_ok) {
        const auto err = static_cast<int>(::ERR_get_error());
        co_return std::unexpected{
            AppError{ErrorCode::Io, "tls SNI set failed: " + std::to_string(err)}};
    }
    sock->stream.set_verify_mode(asio::ssl::verify_peer);
    sock->stream.set_verify_callback(asio::ssl::host_name_verification(key.host));

    boost::system::error_code ec;
    tcp::resolver resolver{exec};
    auto endpoints = co_await resolver.async_resolve(
        key.host, key.port, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "dns resolve " + key.host);

    beast::get_lowest_layer(sock->stream).expires_after(timeout);
    co_await beast::get_lowest_layer(sock->stream)
        .async_connect(endpoints, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "tcp connect " + key.host);

    beast::get_lowest_layer(sock->stream).expires_after(timeout);
    co_await sock->stream.async_handshake(asio::ssl::stream_base::client,
                                          asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "tls handshake " + key.host);

    co_return sock;
}

// Send request + read response on either stream type. Templated on the
// stream so the same code path serves plain and TLS.
template <typename Stream>
asio::awaitable<Result<std::pair<HttpResponse, bool /*reusable*/>>> exchange(
    Stream& stream, bh::request<bh::string_body> req, std::chrono::milliseconds timeout) {
    boost::system::error_code ec;

    beast::get_lowest_layer(stream).expires_after(timeout);
    co_await bh::async_write(stream, req, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "http write");

    beast::flat_buffer buffer;
    bh::response<bh::string_body> resp;
    beast::get_lowest_layer(stream).expires_after(timeout);
    co_await bh::async_read(stream, buffer, resp, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "http read");

    HttpResponse out;
    out.status_code = static_cast<int>(resp.result_int());
    out.headers = extract_headers(resp.base());
    out.body = std::move(resp.body());
    out.duration = std::chrono::milliseconds{0};

    const bool reusable = resp.keep_alive();
    co_return std::make_pair(std::move(out), reusable);
}

// Streaming response → file via response_parser<file_body>. Returns the
// headers; the body is written incrementally to `dest`.
template <typename Stream>
asio::awaitable<Result<std::pair<HttpResponse, bool /*reusable*/>>> exchange_to_file(
    Stream& stream,
    bh::request<bh::string_body> req,
    const std::filesystem::path& dest,
    std::function<void(std::int64_t, std::int64_t)> on_progress,
    std::chrono::milliseconds timeout) {
    boost::system::error_code ec;

    beast::get_lowest_layer(stream).expires_after(timeout);
    co_await bh::async_write(stream, req, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "http write");

    beast::flat_buffer buffer;
    bh::response_parser<bh::buffer_body> parser;
    parser.body_limit(boost::none);

    beast::get_lowest_layer(stream).expires_after(timeout);
    co_await bh::async_read_header(
        stream, buffer, parser, asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
        co_return translate_ec(ec, "http read header");

    HttpResponse out;
    out.status_code = static_cast<int>(parser.get().result_int());
    out.headers = extract_headers(parser.get().base());

    // Open destination for write only on 2xx success — otherwise the caller
    // wants the error body back as a string (we'll buffer it here).
    const bool success = out.status_code >= 200 && out.status_code < 300;
    std::int64_t total = -1;
    if (auto cl = find_header(parser.get().base(), "content-length")) {
        try {
            total = std::stoll(*cl);
        } catch (...) {
            total = -1;
        }
    }

    std::ofstream file_out;
    if (success) {
        file_out.open(dest, std::ios::binary | std::ios::trunc);
        if (!file_out) {
            co_return std::unexpected{AppError{ErrorCode::Io, "open for write: " + dest.string()}};
        }
    }

    std::array<char, 64 * 1024> chunk{};
    std::int64_t downloaded = 0;
    auto last_cb = std::chrono::steady_clock::now() - kProgressMinInterval;

    while (!parser.is_done()) {
        parser.get().body().data = chunk.data();
        parser.get().body().size = chunk.size();

        beast::get_lowest_layer(stream).expires_after(timeout);
        co_await bh::async_read_some(
            stream, buffer, parser, asio::redirect_error(asio::use_awaitable, ec));
        if (ec == bh::error::need_buffer)
            ec = {};
        if (ec)
            co_return translate_ec(ec, "http read body");

        const std::size_t got = chunk.size() - parser.get().body().size;
        if (got > 0) {
            if (success) {
                file_out.write(chunk.data(), static_cast<std::streamsize>(got));
                if (!file_out) {
                    co_return std::unexpected{AppError{ErrorCode::Io, "write: " + dest.string()}};
                }
            } else {
                out.body.append(chunk.data(), got);
            }
            downloaded += static_cast<std::int64_t>(got);
            const auto now = std::chrono::steady_clock::now();
            if (on_progress && (now - last_cb) >= kProgressMinInterval) {
                try {
                    on_progress(downloaded, total);
                } catch (...) {
                }
                last_cb = now;
            }
        }
    }

    if (success && on_progress) {
        try {
            on_progress(downloaded, total);
        } catch (...) {
        }
    }
    if (success)
        file_out.close();

    const bool reusable = parser.get().keep_alive();
    co_return std::make_pair(std::move(out), reusable);
}

} // namespace

// ===========================================================================
// BeastHttpClient
// ===========================================================================

BeastHttpClient::BeastHttpClient(asio::any_io_executor exec)
    : exec_{std::move(exec)},
      ssl_ctx_{asio::ssl::context::tls_client},
      pool_{std::make_shared<ConnectionPool>(exec_)} {
    ssl_ctx_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2
                         | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1
                         | asio::ssl::context::no_tlsv1_1);
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(asio::ssl::verify_peer);
    Logger::debug("BeastHttpClient: ready (pool max {} endpoints x {} sockets)",
                  kMaxEndpoints,
                  kMaxSocketsPerEndpoint);
}

asio::awaitable<Result<HttpResponse>> BeastHttpClient::request(HttpRequest req) {
    const auto start = std::chrono::steady_clock::now();

    HttpMethod method = req.method;
    std::string url = req.url;
    std::vector<HttpHeader> headers = std::move(req.headers);
    std::string body = std::move(req.body);
    auto timeout = req.timeout;
    if (timeout <= std::chrono::milliseconds{0}) {
        timeout = std::chrono::milliseconds{30000};
    }

    // Track the original auth context so we can strip Authorization on
    // cross-origin redirect.
    auto parsed_orig = parse_url(url);
    if (!parsed_orig) {
        co_return std::unexpected{AppError{ErrorCode::InvalidArgument, "malformed URL: " + url}};
    }

    bool redirected = false;
    for (std::size_t hop = 0; hop <= kMaxRedirects; ++hop) {
        auto parsed = parse_url(url);
        if (!parsed) {
            co_return std::unexpected{
                AppError{ErrorCode::InvalidArgument, "malformed URL (redirect): " + url}};
        }

        EndpointKey key{parsed->https, parsed->host, parsed->port};
        std::string host_with_port = parsed->host;
        if ((parsed->https && parsed->port != "443") || (!parsed->https && parsed->port != "80")) {
            host_with_port += ":" + parsed->port;
        }

        bh::request<bh::string_body> hreq{to_verb(method), parsed->target, 11};
        const bool has_body =
            method == HttpMethod::Post || method == HttpMethod::Put || method == HttpMethod::Patch;
        if (has_body)
            hreq.body() = body;
        // Strip Authorization on cross-origin redirect.
        if (redirected && !same_origin(*parsed_orig, *parsed)) {
            std::erase_if(headers, [](const HttpHeader& h) {
                return iequals(h.name, "authorization");
            });
        }
        apply_headers(hreq, headers, host_with_port, has_body);

        std::pair<HttpResponse, bool> outcome;

        if (parsed->https) {
            auto sock = pool_->take_tls(key);
            if (!sock) {
                auto opened = co_await open_tls(exec_, ssl_ctx_, key, timeout);
                if (!opened)
                    co_return std::unexpected(opened.error());
                sock = std::move(*opened);
            }
            auto ex = co_await exchange(sock->stream, std::move(hreq), timeout);
            if (!ex) {
                // Socket may be poisoned; drop it.
                co_return std::unexpected(ex.error());
            }
            outcome = std::move(*ex);
            if (outcome.second) {
                pool_->return_tls(key, std::move(sock));
            } else {
                // Peer wants close; do a polite shutdown but don't block.
                boost::system::error_code ec;
                sock->stream.shutdown(ec);
            }
        } else {
            auto sock = pool_->take_plain(key);
            if (!sock) {
                auto opened = co_await open_plain(exec_, key, timeout);
                if (!opened)
                    co_return std::unexpected(opened.error());
                sock = std::move(*opened);
            }
            auto ex = co_await exchange(sock->stream, std::move(hreq), timeout);
            if (!ex)
                co_return std::unexpected(ex.error());
            outcome = std::move(*ex);
            if (outcome.second) {
                pool_->return_plain(key, std::move(sock));
            } else {
                boost::system::error_code ec;
                sock->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            }
        }

        HttpResponse& resp = outcome.first;
        const int code = resp.status_code;

        // Redirect handling.
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
            && hop < kMaxRedirects) {
            std::optional<std::string> location;
            for (const auto& h : resp.headers) {
                if (iequals(h.name, "location")) {
                    location = h.value;
                    break;
                }
            }
            if (!location) {
                // No Location header — surface as-is.
                resp.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                co_return resp;
            }
            auto next = resolve_redirect(*parsed, *location);
            if (!next) {
                co_return std::unexpected{
                    AppError{ErrorCode::Network, "redirect target malformed: " + *location}};
            }
            url = *next;
            if (code == 301 || code == 302 || code == 303) {
                // RFC 7231: 303 always, 301/302 commonly seen as method-change.
                method = HttpMethod::Get;
                body.clear();
            }
            redirected = true;
            continue;
        }

        resp.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        co_return resp;
    }

    co_return std::unexpected{
        AppError{ErrorCode::Network,
                 "too many redirects (max " + std::to_string(kMaxRedirects) + "): " + url}};
}

asio::awaitable<Result<HttpResponse>> BeastHttpClient::get(std::string url,
                                                           std::vector<HttpHeader> headers) {
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.url = std::move(url);
    req.headers = std::move(headers);
    co_return co_await request(std::move(req));
}

asio::awaitable<Result<HttpResponse>> BeastHttpClient::post(std::string url,
                                                            std::string body,
                                                            std::vector<HttpHeader> headers) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = std::move(url);
    req.body = std::move(body);
    req.headers = std::move(headers);
    co_return co_await request(std::move(req));
}

asio::awaitable<Result<HttpResponse>> BeastHttpClient::download_to_file(
    std::string url,
    std::filesystem::path dest,
    std::function<void(std::int64_t, std::int64_t)> on_progress,
    std::vector<HttpHeader> headers) {
    const auto start = std::chrono::steady_clock::now();
    std::chrono::milliseconds timeout{60000};

    auto parsed_orig = parse_url(url);
    if (!parsed_orig) {
        co_return std::unexpected{AppError{ErrorCode::InvalidArgument, "malformed URL: " + url}};
    }

    HttpMethod method = HttpMethod::Get;
    std::string body;

    bool redirected = false;
    for (std::size_t hop = 0; hop <= kMaxRedirects; ++hop) {
        auto parsed = parse_url(url);
        if (!parsed) {
            co_return std::unexpected{
                AppError{ErrorCode::InvalidArgument, "malformed URL (redirect): " + url}};
        }
        EndpointKey key{parsed->https, parsed->host, parsed->port};
        std::string host_with_port = parsed->host;
        if ((parsed->https && parsed->port != "443") || (!parsed->https && parsed->port != "80")) {
            host_with_port += ":" + parsed->port;
        }

        bh::request<bh::string_body> hreq{to_verb(method), parsed->target, 11};
        if (redirected && !same_origin(*parsed_orig, *parsed)) {
            std::erase_if(headers, [](const HttpHeader& h) {
                return iequals(h.name, "authorization");
            });
        }
        apply_headers(hreq, headers, host_with_port, /*has_body*/ false);

        std::pair<HttpResponse, bool> outcome;

        if (parsed->https) {
            auto sock = pool_->take_tls(key);
            if (!sock) {
                auto opened = co_await open_tls(exec_, ssl_ctx_, key, timeout);
                if (!opened)
                    co_return std::unexpected(opened.error());
                sock = std::move(*opened);
            }
            auto ex = co_await exchange_to_file(
                sock->stream, std::move(hreq), dest, on_progress, timeout);
            if (!ex)
                co_return std::unexpected(ex.error());
            outcome = std::move(*ex);
            if (outcome.second) {
                pool_->return_tls(key, std::move(sock));
            } else {
                boost::system::error_code ec;
                sock->stream.shutdown(ec);
            }
        } else {
            auto sock = pool_->take_plain(key);
            if (!sock) {
                auto opened = co_await open_plain(exec_, key, timeout);
                if (!opened)
                    co_return std::unexpected(opened.error());
                sock = std::move(*opened);
            }
            auto ex = co_await exchange_to_file(
                sock->stream, std::move(hreq), dest, on_progress, timeout);
            if (!ex)
                co_return std::unexpected(ex.error());
            outcome = std::move(*ex);
            if (outcome.second) {
                pool_->return_plain(key, std::move(sock));
            } else {
                boost::system::error_code ec;
                sock->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            }
        }

        HttpResponse& resp = outcome.first;
        const int code = resp.status_code;

        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
            && hop < kMaxRedirects) {
            std::optional<std::string> location;
            for (const auto& h : resp.headers) {
                if (iequals(h.name, "location")) {
                    location = h.value;
                    break;
                }
            }
            if (!location) {
                resp.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                co_return resp;
            }
            auto next = resolve_redirect(*parsed, *location);
            if (!next) {
                co_return std::unexpected{
                    AppError{ErrorCode::Network, "redirect target malformed: " + *location}};
            }
            url = *next;
            redirected = true;
            continue;
        }

        resp.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        co_return resp;
    }

    co_return std::unexpected{
        AppError{ErrorCode::Network,
                 "too many redirects (max " + std::to_string(kMaxRedirects) + "): " + url}};
}

} // namespace cmlb::infrastructure::http
