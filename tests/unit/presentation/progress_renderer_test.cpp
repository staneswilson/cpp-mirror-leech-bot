// ---------------------------------------------------------------------------
// progress_renderer_test.cpp - unit tests for live status message rendering.
// ---------------------------------------------------------------------------

#include "stub_messenger.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/presentation/progress_renderer.hpp>

namespace asio = boost::asio;

using cmlb::domain::ChatId;
using cmlb::domain::Gid;
using cmlb::infrastructure::download::DownloadState;
using cmlb::infrastructure::download::DownloadStatus;
using cmlb::presentation::ProgressRenderer;
using cmlb::test_support::StubMessenger;

namespace {

class CountingMetrics final : public cmlb::infrastructure::system::SystemMetrics {
public:
    [[nodiscard]] cmlb::infrastructure::system::SystemSnapshot snapshot() const override {
        snapshot_calls_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    [[nodiscard]] int snapshot_calls() const noexcept {
        return snapshot_calls_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<int> snapshot_calls_{0};
};

template <typename Factory>
auto run_on(asio::io_context& ctx, Factory&& f) {
    auto fut = asio::co_spawn(ctx, std::forward<Factory>(f), asio::use_future);
    ctx.run();
    auto value = fut.get();
    ctx.restart();
    return value;
}

[[nodiscard]] DownloadStatus make_status() {
    DownloadStatus status;
    status.id = Gid{std::string{"gid-1"}};
    status.name = "file.iso";
    status.state = DownloadState::Downloading;
    status.total_bytes = 1024;
    status.downloaded_bytes = 512;
    status.download_speed_bps = 128;
    status.eta = std::chrono::seconds{4};
    return status;
}

} // namespace

TEST_CASE("ProgressRenderer drops periodic renders inside the throttle window",
          "[presentation][progress]") {
    asio::io_context ctx;
    StubMessenger messenger;
    CountingMetrics metrics;
    ProgressRenderer renderer{messenger,
                              metrics,
                              std::chrono::steady_clock::now(),
                              ctx.get_executor(),
                              std::chrono::hours{1}};
    const std::array active{make_status()};
    auto first = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await renderer.render(ChatId{-100123}, active);
    });
    REQUIRE(first.has_value());
    REQUIRE(metrics.snapshot_calls() == 1);
    REQUIRE(messenger.sends().size() == 1);
    REQUIRE(messenger.edits().empty());

    auto second = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await renderer.render(ChatId{-100123}, active);
    });
    REQUIRE(second.has_value());
    CHECK(metrics.snapshot_calls() == 1);
    CHECK(messenger.sends().size() == 1);
    CHECK(messenger.edits().empty());
}

TEST_CASE("ProgressRenderer force_refresh edits the cached status message instead of sending "
          "another chat message",
          "[presentation][progress]") {
    asio::io_context ctx;
    StubMessenger messenger;
    CountingMetrics metrics;
    ProgressRenderer renderer{messenger,
                              metrics,
                              std::chrono::steady_clock::now(),
                              ctx.get_executor(),
                              std::chrono::hours{1}};
    const std::array active{make_status()};
    auto refreshed_status = make_status();
    refreshed_status.downloaded_bytes = 768;
    const std::array refreshed_active{refreshed_status};

    auto first = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await renderer.render(ChatId{-100123}, active);
    });
    REQUIRE(first.has_value());
    REQUIRE(messenger.sends().size() == 1);
    const auto status_message = messenger.sends().front().assigned;

    auto refreshed = run_on(ctx, [&]() -> asio::awaitable<cmlb::core::Result<void>> {
        co_return co_await renderer.force_refresh(ChatId{-100123}, refreshed_active);
    });
    REQUIRE(refreshed.has_value());

    CHECK(messenger.sends().size() == 1);
    REQUIRE(messenger.edits().size() == 1);
    CHECK(messenger.edits().front().msg == status_message);
}
