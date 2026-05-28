// ---------------------------------------------------------------------------
// tests/integration/qbittorrent_downloader_test.cpp
//
// Live qBittorrent integration. Skipped unless `CMLB_E2E_QBIT=1`. Expects a
// running qBittorrent Web UI at `CMLB_E2E_QBIT_URL` (default
// `http://127.0.0.1:8080`) with credentials from `CMLB_E2E_QBIT_USER` /
// `CMLB_E2E_QBIT_PASS` (default `admin` / `adminadmin`).
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdlib>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/core/configuration.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/infrastructure/download/qbittorrent_downloader.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>

using namespace std::chrono_literals;
using cmlb::core::Executor;
using cmlb::core::QbittorrentConfig;
using cmlb::infrastructure::download::DownloadOptions;
using cmlb::infrastructure::download::QbittorrentDownloader;
using cmlb::infrastructure::http::BeastHttpClient;

namespace {

[[nodiscard]] bool e2e_enabled() {
    const char* flag = std::getenv("CMLB_E2E_QBIT");
    return flag != nullptr && std::string{flag} == "1";
}

[[nodiscard]] std::string getenv_or(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return v ? std::string{v} : fallback;
}

} // namespace

TEST_CASE("qbittorrent downloader: logs in and reports global stats",
          "[integration][qbittorrent][!mayfail]") {
    if (!e2e_enabled()) {
        SKIP("CMLB_E2E_QBIT=1 not set");
    }

    Executor exec{2};
    BeastHttpClient http{exec.get_executor()};
    QbittorrentConfig cfg;
    cfg.url = getenv_or("CMLB_E2E_QBIT_URL", "http://127.0.0.1:8080");
    cfg.username = getenv_or("CMLB_E2E_QBIT_USER", "admin");
    cfg.password = getenv_or("CMLB_E2E_QBIT_PASS", "adminadmin");

    QbittorrentDownloader downloader{exec, cfg, http};
    REQUIRE(downloader.client_name() == "qbittorrent");

    auto fut = boost::asio::co_spawn(
        exec.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            auto stats = co_await downloader.global_stats();
            co_return stats.has_value();
        },
        boost::asio::use_future);

    REQUIRE(fut.get());
    REQUIRE(downloader.is_connected());
}

TEST_CASE("qbittorrent downloader: add_uri returns a hash",
          "[integration][qbittorrent][!mayfail]") {
    if (!e2e_enabled()) {
        SKIP("CMLB_E2E_QBIT=1 not set");
    }

    Executor exec{2};
    BeastHttpClient http{exec.get_executor()};
    QbittorrentConfig cfg;
    cfg.url = getenv_or("CMLB_E2E_QBIT_URL", "http://127.0.0.1:8080");
    cfg.username = getenv_or("CMLB_E2E_QBIT_USER", "admin");
    cfg.password = getenv_or("CMLB_E2E_QBIT_PASS", "adminadmin");

    QbittorrentDownloader downloader{exec, cfg, http};

    // Ubuntu 22.04 desktop release is small and well-seeded.
    const std::string magnet =
        getenv_or("CMLB_E2E_QBIT_TEST_MAGNET",
                  "magnet:?xt=urn:btih:2c6b6858d61da9543d4231a71db4b1c9264b0685"
                  "&dn=ubuntu-22.04-desktop-amd64.iso");

    auto fut = boost::asio::co_spawn(
        exec.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            auto add_res = co_await downloader.add_uri(magnet, DownloadOptions{});
            if (!add_res)
                co_return false;
            // Immediately remove so we don't leave the daemon downloading.
            auto rm = co_await downloader.remove(*add_res, /*delete_files=*/true);
            co_return rm.has_value();
        },
        boost::asio::use_future);

    REQUIRE(fut.get());
}
