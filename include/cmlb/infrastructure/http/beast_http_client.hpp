#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>

#include <cmlb/core/error.hpp>

namespace cmlb::infrastructure::http {

/// Supported HTTP verbs. `Delete_` is suffixed to avoid colliding with the
/// `DELETE` macro that some Windows headers (`winnt.h`) define.
enum class HttpMethod {
    Get,
    Post,
    Put,
    Delete_,
    Patch,
    Head
};

/// Case-preserving header name/value pair.
///
/// Headers are stored as-is and compared case-insensitively when needed by
/// the implementation (for redirect filtering, content-length lookups, ...).
struct HttpHeader {
    /// Header field name (e.g. `"Content-Type"`).
    std::string name;
    /// Header field value.
    std::string value;
};

/// Complete description of an outgoing HTTP/HTTPS request.
///
/// Construct one of these and pass it to `BeastHttpClient::request()`. The
/// convenience wrappers (`get`, `post`, `download_to_file`) fill this struct
/// internally so callers rarely need to instantiate it directly.
struct HttpRequest {
    /// Verb to send. Defaults to `Get` for the common case.
    HttpMethod method = HttpMethod::Get;
    /// Full URL including scheme (`http://` or `https://`).
    /// Malformed URLs cause `request()` to return `InvalidArgument`.
    std::string url;
    /// Additional headers to send. `Host`, `Content-Length`, `User-Agent`
    /// and `Connection` are set automatically by the implementation.
    std::vector<HttpHeader> headers;
    /// Request body. Ignored for `Get` and `Head`.
    std::string body;
    /// Maximum total time allowed for the entire exchange (connect + send +
    /// receive). Defaults to 30 seconds.
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

/// Parsed HTTP response.
struct HttpResponse {
    /// Numeric status code (e.g. 200, 404, 502).
    int status_code = 0;
    /// Headers returned by the server, preserved in arrival order.
    std::vector<HttpHeader> headers;
    /// Decoded body. For streaming downloads this is empty.
    std::string body;
    /// Wall-clock duration from `request()` call to completion.
    std::chrono::milliseconds duration{0};
};

/// Asynchronous HTTP/1.1 client built on Boost.Beast.
///
/// Behaviour:
/// * `http://` URLs use a plain TCP stream; `https://` URLs use OpenSSL with
///   SNI enabled and `verify_peer` against the system trust store.
/// * Redirects (301/302/303/307/308) are followed automatically up to a hard
///   limit of 5 hops. The `Authorization` header is stripped on cross-origin
///   redirects to avoid credential leakage.
/// * Per-request timeouts are honored via `boost::beast::tcp_stream::expires_after`.
///
/// The client is cheap to construct; reuse a single instance per executor
/// for connection reuse to be added in a future iteration.
class BeastHttpClient {
public:
    /// Constructs a client bound to `exec`. The executor must outlive every
    /// outstanding awaitable returned by this object.
    explicit BeastHttpClient(boost::asio::any_io_executor exec);

    /// Sends `req` and returns the response on success. On failure returns
    /// an `AppError` with one of: `InvalidArgument` (malformed URL),
    /// `Network` (DNS/connect/transport), `Timeout` (deadline exceeded),
    /// `Io` (TLS or read/write error).
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<HttpResponse>> request(HttpRequest req);

    /// Convenience GET. Equivalent to `request({Get, url, headers, {}, default_timeout})`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<HttpResponse>> get(
        std::string url, std::vector<HttpHeader> headers = {});

    /// Convenience POST.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<HttpResponse>> post(
        std::string url, std::string body, std::vector<HttpHeader> headers = {});

    /// Streaming download: GETs `url` and writes the response body to `dest`
    /// in chunks via Beast's `response_parser<file_body>`. The body is never
    /// held in memory in its entirety.
    ///
    /// `on_progress`, if non-null, is invoked periodically as bytes arrive
    /// with `(downloaded_bytes, total_bytes)`. `total_bytes` is `-1` when
    /// the server did not send a `Content-Length` header.
    ///
    /// On success returns an `HttpResponse` whose `body` is empty but whose
    /// `status_code`/`headers`/`duration` reflect the exchange.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<HttpResponse>> download_to_file(
        std::string url,
        std::filesystem::path dest,
        std::function<void(std::int64_t, std::int64_t)> on_progress = nullptr,
        std::vector<HttpHeader> headers = {});

private:
    boost::asio::any_io_executor exec_;
    // SSL context is created lazily on first HTTPS use and cached.
    boost::asio::ssl::context ssl_ctx_;
    // Idle keep-alive sockets, keyed on (scheme, host, port). Lives in the
    // .cpp; the header only sees the forward declaration.
    std::shared_ptr<class ConnectionPool> pool_;
};

} // namespace cmlb::infrastructure::http
