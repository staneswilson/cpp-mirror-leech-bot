// ---------------------------------------------------------------------------
// tests/integration/aria2_downloader_test.cpp
//
// Live aria2c integration. Skipped unless `CMLB_E2E_ARIA2=1` is set. The
// test assumes an aria2c daemon is listening at the URL given in
// `CMLB_E2E_ARIA2_URL` (default `ws://127.0.0.1:6800/jsonrpc`) with secret
// from `CMLB_E2E_ARIA2_SECRET` (default empty).
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/infrastructure/download/aria2_downloader.hpp>

using namespace std::chrono_literals;
using cmlb::core::Aria2Config;
using cmlb::core::Executor;
using cmlb::infrastructure::download::Aria2Downloader;
using cmlb::infrastructure::download::DownloadOptions;
using cmlb::infrastructure::download::DownloadState;

namespace {

[[nodiscard]] bool e2e_enabled() {
    const char* flag = std::getenv("CMLB_E2E_ARIA2");
    return flag != nullptr && std::string{flag} == "1";
}

[[nodiscard]] std::string getenv_or(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return v ? std::string{v} : fallback;
}

}  // namespace

TEST_CASE("aria2 downloader: connects to live daemon",
          "[integration][aria2][!mayfail]") {
    if (!e2e_enabled()) {
        SKIP("CMLB_E2E_ARIA2=1 not set");
    }

    Executor exec{2};
    Aria2Config cfg;
    cfg.rpc_url = getenv_or("CMLB_E2E_ARIA2_URL", "ws://127.0.0.1:6800/jsonrpc");
    cfg.secret  = getenv_or("CMLB_E2E_ARIA2_SECRET", "");

    Aria2Downloader downloader{exec, cfg};

    // Give the background reconnect loop a moment to establish the WS.
    for (int i = 0; i < 50 && !downloader.is_connected(); ++i) {
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(downloader.is_connected());

    REQUIRE(downloader.client_name() == "aria2");
}

TEST_CASE("aria2 downloader: add_uri + status round-trip",
          "[integration][aria2][!mayfail]") {
    if (!e2e_enabled()) {
        SKIP("CMLB_E2E_ARIA2=1 not set");
    }

    Executor exec{2};
    Aria2Config cfg;
    cfg.rpc_url = getenv_or("CMLB_E2E_ARIA2_URL", "ws://127.0.0.1:6800/jsonrpc");
    cfg.secret  = getenv_or("CMLB_E2E_ARIA2_SECRET", "");

    Aria2Downloader downloader{exec, cfg};

    for (int i = 0; i < 50 && !downloader.is_connected(); ++i) {
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(downloader.is_connected());

    // A small public test asset. Replace with a fixture-served URL when the
    // shared HTTP server fixture lands.
    const std::string url =
        getenv_or("CMLB_E2E_ARIA2_TEST_URL",
                  "https://www.google.com/robots.txt");

    DownloadOptions options;
    auto fut = boost::asio::co_spawn(
        exec.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            auto gid_res = co_await downloader.add_uri(url, options);
            if (!gid_res) co_return false;

            // Poll status until terminal or timeout.
            for (int i = 0; i < 100; ++i) {
                auto status_res = co_await downloader.status(*gid_res);
                if (!status_res) co_return false;
                if (status_res->state == DownloadState::Complete) co_return true;
                if (status_res->state == DownloadState::Error)    co_return false;
                co_await boost::asio::steady_timer{
                    co_await boost::asio::this_coro::executor,
                    100ms}.async_wait(boost::asio::use_awaitable);
            }
            co_return false;
        },
        boost::asio::use_future);

    REQUIRE(fut.get());
}
