// ---------------------------------------------------------------------------
// show_stats_test.cpp - unit tests for the /stats use case.
// ---------------------------------------------------------------------------

#include "stub_downloader.hpp"

#include <chrono>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/application/show_stats.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>

namespace asio = boost::asio;

using cmlb::application::ShowStats;
using cmlb::application::StatsReport;
using cmlb::application::StatsRequest;
using cmlb::core::ErrorCode;
using cmlb::infrastructure::download::GlobalStats;
using cmlb::test_support::StubDownloader;

namespace {

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

} // namespace

TEST_CASE("ShowStats sums active downloads across aria2 and qBittorrent",
          "[application][show_stats]") {
    StubDownloader aria2;
    StubDownloader qbit;
    cmlb::infrastructure::system::SystemMetrics metrics;

    GlobalStats aria2_stats;
    aria2_stats.active_count = 2;
    aria2.set_global_stats(aria2_stats);

    GlobalStats qbit_stats;
    qbit_stats.active_count = 3;
    qbit.set_global_stats(qbit_stats);

    ShowStats use_case{aria2, qbit, metrics, std::chrono::steady_clock::now()};
    asio::io_context ctx;

    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<StatsReport>> {
        co_return co_await use_case.execute(StatsRequest{});
    });

    REQUIRE(result.has_value());
    CHECK(result->active_downloads == 5);
    CHECK(result->unavailable_downloaders.empty());
}

TEST_CASE("ShowStats keeps host stats when one downloader stats call fails",
          "[application][show_stats]") {
    StubDownloader aria2;
    StubDownloader qbit;
    cmlb::infrastructure::system::SystemMetrics metrics;

    GlobalStats qbit_stats;
    qbit_stats.active_count = 4;
    qbit.set_global_stats(qbit_stats);
    aria2.set_global_stats_error({ErrorCode::Network, "aria2 unavailable"});

    ShowStats use_case{aria2, qbit, metrics, std::chrono::steady_clock::now()};
    asio::io_context ctx;

    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<StatsReport>> {
        co_return co_await use_case.execute(StatsRequest{});
    });

    REQUIRE(result.has_value());
    CHECK(result->active_downloads == 4);
    REQUIRE(result->unavailable_downloaders.size() == 1);
    CHECK(result->unavailable_downloaders.front() == "aria2");
}

TEST_CASE("ShowStats still returns uptime and metrics when both downloaders fail",
          "[application][show_stats]") {
    StubDownloader aria2;
    StubDownloader qbit;
    cmlb::infrastructure::system::SystemMetrics metrics;

    aria2.set_global_stats_error({ErrorCode::Network, "aria2 unavailable"});
    qbit.set_global_stats_error({ErrorCode::QbittorrentApi, "qbit unavailable"});

    ShowStats use_case{aria2, qbit, metrics, std::chrono::steady_clock::now()};
    asio::io_context ctx;

    auto result = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<StatsReport>> {
        co_return co_await use_case.execute(StatsRequest{});
    });

    REQUIRE(result.has_value());
    CHECK(result->active_downloads == 0);
    REQUIRE(result->unavailable_downloaders.size() == 2);
    CHECK(result->unavailable_downloaders[0] == "aria2");
    CHECK(result->unavailable_downloaders[1] == "qbittorrent");
}
