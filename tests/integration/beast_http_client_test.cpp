// Integration tests for cmlb::infrastructure::http::BeastHttpClient.
//
// Spins up a minimal Beast-based HTTP/1.1 server on a kernel-assigned port,
// then exercises the client against it. Covers:
//   * 200 OK GET
//   * 404 Not Found
//   * Redirect chain (302 -> 302 -> 200)
//   * Timeout (server stalls)

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <cmlb/infrastructure/http/beast_http_client.hpp>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = asio::ip::tcp;

using cmlb::infrastructure::http::BeastHttpClient;
using cmlb::infrastructure::http::HttpResponse;
using Catch::Matchers::ContainsSubstring;

namespace {

// Per-test server context.
struct TestServer {
    asio::io_context io;
    std::thread thread;
    std::uint16_t port = 0;
    std::atomic<bool> running{true};

    explicit TestServer(std::function<void(http::request<http::string_body>&,
                                           http::response<http::string_body>&)> handler) {
        tcp::acceptor acceptor{io, tcp::endpoint{tcp::v4(), 0}};
        port = acceptor.local_endpoint().port();

        auto handler_ptr = std::make_shared<decltype(handler)>(std::move(handler));

        auto session = [handler_ptr](tcp::socket sock) -> asio::awaitable<void> {
            try {
                beast::tcp_stream stream{std::move(sock)};
                beast::flat_buffer buf;
                http::request<http::string_body> req;
                co_await http::async_read(stream, buf, req, asio::use_awaitable);

                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::server, "cmlb-test/1.0");
                res.keep_alive(false);
                (*handler_ptr)(req, res);
                res.prepare_payload();
                co_await http::async_write(stream, res, asio::use_awaitable);
                beast::error_code ec;
                stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            } catch (...) {
                // Best-effort — failures inside the test server are visible
                // via the client-side assertions.
            }
        };

        auto loop = [&, acceptor = std::move(acceptor),
                     session]() mutable -> asio::awaitable<void> {
            while (running.load()) {
                boost::system::error_code ec;
                auto sock = co_await acceptor.async_accept(
                    asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    co_return;
                }
                asio::co_spawn(io, session(std::move(sock)), asio::detached);
            }
        };

        asio::co_spawn(io, loop(), asio::detached);
        thread = std::thread{[this] { io.run(); }};
    }

    ~TestServer() {
        running.store(false);
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

template <typename F>
auto run_awaitable(F&& f) {
    asio::io_context io;
    auto fut = asio::co_spawn(io.get_executor(),
                              std::forward<F>(f)(io.get_executor()),
                              asio::use_future);
    io.run();
    return fut.get();
}

}  // namespace

TEST_CASE("BeastHttpClient GET returns 200 with body",
          "[integration][http][get]") {
    TestServer server{[](auto& /*req*/, auto& res) {
        res.result(http::status::ok);
        res.set(http::field::content_type, "text/plain");
        res.body() = "hello-cmlb";
    }};

    auto result = run_awaitable([&](asio::any_io_executor exec)
                                    -> asio::awaitable<cmlb::core::Result<HttpResponse>> {
        BeastHttpClient client{exec};
        co_return co_await client.get(server.base_url() + "/hello");
    });

    REQUIRE(result.has_value());
    CHECK(result->status_code == 200);
    CHECK(result->body == "hello-cmlb");
}

TEST_CASE("BeastHttpClient surfaces 404 responses without erroring",
          "[integration][http][404]") {
    TestServer server{[](auto& /*req*/, auto& res) {
        res.result(http::status::not_found);
        res.body() = "missing";
    }};

    auto result = run_awaitable([&](asio::any_io_executor exec)
                                    -> asio::awaitable<cmlb::core::Result<HttpResponse>> {
        BeastHttpClient client{exec};
        co_return co_await client.get(server.base_url() + "/nope");
    });

    REQUIRE(result.has_value());
    CHECK(result->status_code == 404);
    CHECK(result->body == "missing");
}

TEST_CASE("BeastHttpClient follows redirect chains",
          "[integration][http][redirect]") {
    TestServer server{[](auto& req, auto& res) {
        if (req.target() == "/one") {
            res.result(http::status::found);
            res.set(http::field::location, "/two");
        } else if (req.target() == "/two") {
            res.result(http::status::found);
            res.set(http::field::location, "/three");
        } else {
            res.result(http::status::ok);
            res.body() = "final";
        }
    }};

    auto result = run_awaitable([&](asio::any_io_executor exec)
                                    -> asio::awaitable<cmlb::core::Result<HttpResponse>> {
        BeastHttpClient client{exec};
        co_return co_await client.get(server.base_url() + "/one");
    });

    REQUIRE(result.has_value());
    CHECK(result->status_code == 200);
    CHECK(result->body == "final");
}

TEST_CASE("BeastHttpClient honors per-request timeout",
          "[integration][http][timeout]") {
    // Server that never replies — bind a socket and accept without responding.
    asio::io_context srv_io;
    tcp::acceptor acceptor{srv_io, tcp::endpoint{tcp::v4(), 0}};
    auto port = acceptor.local_endpoint().port();
    std::atomic<bool> alive{true};

    std::thread srv_thread{[&] {
        try {
            while (alive.load()) {
                boost::system::error_code ec;
                tcp::socket s{srv_io};
                acceptor.accept(s, ec);
                if (ec) break;
                // Hold the connection open without writing anything.
                while (alive.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        } catch (...) {
        }
    }};

    auto result = run_awaitable([&, port](asio::any_io_executor exec)
                                    -> asio::awaitable<cmlb::core::Result<HttpResponse>> {
        BeastHttpClient client{exec};
        cmlb::infrastructure::http::HttpRequest req;
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/slow";
        req.timeout = std::chrono::milliseconds(200);
        co_return co_await client.request(std::move(req));
    });

    alive.store(false);
    boost::system::error_code ec;
    acceptor.close(ec);
    if (srv_thread.joinable()) {
        srv_thread.join();
    }

    REQUIRE_FALSE(result.has_value());
    // Either Network (connect aborted by deadline) or Io (read aborted).
    CHECK((result.error().code == cmlb::core::ErrorCode::Io
           || result.error().code == cmlb::core::ErrorCode::Network
           || result.error().code == cmlb::core::ErrorCode::Timeout));
}
