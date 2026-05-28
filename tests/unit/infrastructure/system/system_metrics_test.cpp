// Unit tests for cmlb::infrastructure::system::SystemMetrics.
//
// We only assert sane invariants - exact values are platform-dependent and
// non-deterministic. RAM total should be positive and uptime should be
// non-negative for any environment we expect to run on.

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <cmlb/infrastructure/system/system_metrics.hpp>

using cmlb::infrastructure::system::SystemMetrics;
using cmlb::infrastructure::system::SystemSnapshot;

TEST_CASE("SystemMetrics::snapshot returns sane values", "[infrastructure][system][metrics]") {
    SystemMetrics metrics;

    // Let the bot uptime tick over at least a few milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SystemSnapshot snap = metrics.snapshot();

    CHECK(snap.ram_total_bytes > 0);
    CHECK(snap.ram_used_bytes >= 0);
    CHECK(snap.ram_used_bytes <= snap.ram_total_bytes);
    CHECK(snap.bot_uptime.count() >= 0);
    CHECK(snap.system_uptime.count() >= 0);
    CHECK(snap.cpu_usage_percent >= 0.0);
    CHECK(snap.cpu_usage_percent <= 100.0);
    CHECK(snap.disk_total_bytes >= 0);
    CHECK(snap.disk_used_bytes >= 0);
    CHECK(snap.disk_used_bytes <= snap.disk_total_bytes);
}

TEST_CASE("SystemMetrics CPU% delta is non-zero on a busy interval", "[infrastructure][system][metrics][!mayfail]") {
    SystemMetrics metrics;
    // Prime by discarding the first sample.
    (void) metrics.snapshot();

    // Burn a little CPU to push the percentage above noise floor.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    volatile std::uint64_t acc = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++acc;
    }
    (void) acc;

    auto snap = metrics.snapshot();
    // [!mayfail] above lets the suite tolerate this on idle CI machines.
    CHECK(snap.cpu_usage_percent > 0.0);
}
