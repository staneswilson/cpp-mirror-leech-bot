#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/core/formatting.hpp>

using Catch::Matchers::ContainsSubstring;
using cmlb::core::format_bytes;
using cmlb::core::format_decimal_bytes;
using cmlb::core::format_duration;
using cmlb::core::format_eta;
using cmlb::core::format_percent;
using cmlb::core::format_rate;
using cmlb::core::render_progress_bar;

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

TEST_CASE("format_bytes - binary prefixes, two decimals", "[core][formatting]") {
    CHECK(format_bytes(0) == "0 B");
    CHECK(format_bytes(1) == "1 B");
    CHECK(format_bytes(1023) == "1023 B");
    CHECK(format_bytes(1024) == "1.00 KiB");
    CHECK(format_bytes(1536) == "1.50 KiB");
    CHECK(format_bytes(1024LL * 1024) == "1.00 MiB");
    CHECK(format_bytes(1024LL * 1024 * 1024) == "1.00 GiB");
    CHECK(format_bytes(static_cast<std::int64_t>(999) * 1024 * 1024 * 1024) == "999.00 GiB");
    // Largest int64 - must not crash and should produce a PiB-scale output.
    const auto big = format_bytes(std::numeric_limits<std::int64_t>::max());
    CHECK_THAT(big, ContainsSubstring("PiB"));

    // Negative.
    CHECK(format_bytes(-1024) == "-1.00 KiB");
}

TEST_CASE("format_decimal_bytes - SI prefixes", "[core][formatting]") {
    CHECK(format_decimal_bytes(0) == "0 B");
    CHECK(format_decimal_bytes(999) == "999 B");
    CHECK(format_decimal_bytes(1000) == "1.00 KB");
    CHECK(format_decimal_bytes(1500) == "1.50 KB");
    CHECK(format_decimal_bytes(1'000'000) == "1.00 MB");
    CHECK(format_decimal_bytes(1'000'000'000) == "1.00 GB");
}

TEST_CASE("format_duration - h/m/s, day rollover", "[core][formatting]") {
    using namespace std::chrono;
    CHECK(format_duration(seconds{0}) == "0s");
    CHECK(format_duration(seconds{45}) == "45s");
    CHECK(format_duration(seconds{60}) == "1m 0s");
    CHECK(format_duration(seconds{125}) == "2m 5s");
    CHECK(format_duration(seconds{(3 * 3600) + (2 * 60) + 45}) == "3h 2m 45s");
    CHECK(format_duration(seconds{(2 * 86'400) + 3'600}) == "2d 1h 0m 0s");
    // Negative is treated as absolute.
    CHECK(format_duration(seconds{-30}) == "30s");
}

TEST_CASE("format_eta - '--' for zero/negative, '~' prefix otherwise", "[core][formatting]") {
    using namespace std::chrono;
    CHECK(format_eta(seconds{0}) == "--");
    CHECK(format_eta(seconds{-5}) == "--");
    CHECK_THAT(format_eta(seconds{45}), ContainsSubstring("~"));
    CHECK(format_eta(seconds{(2 * 3600) + (3 * 60)}) == "~2h 3m");
    CHECK(format_eta(seconds{125}) == "~2m 5s");
}

TEST_CASE("render_progress_bar - clamping and width handling", "[core][formatting]") {
    CHECK(render_progress_bar(0.0, 10) == "[----------]");
    CHECK(render_progress_bar(1.0, 10) == "[##########]");
    CHECK(render_progress_bar(0.5, 10) == "[#####-----]");
    CHECK(render_progress_bar(-0.1, 10) == "[----------]");
    CHECK(render_progress_bar(1.1, 10) == "[##########]");
    CHECK(render_progress_bar(0.5, 0) == "[]");
    // Custom characters.
    CHECK(render_progress_bar(0.5, 4, '=', '.') == "[==..]");
}

TEST_CASE("format_rate - binary per-second, negatives -> 0", "[core][formatting]") {
    CHECK(format_rate(0) == "0 B/s");
    CHECK(format_rate(1024) == "1.00 KiB/s");
    CHECK(format_rate(static_cast<std::int64_t>(1.23 * 1024 * 1024))
          == format_rate(static_cast<std::int64_t>(1.23 * 1024 * 1024))); // stable
    CHECK_THAT(format_rate(1024LL * 1024), ContainsSubstring("MiB/s"));
    CHECK(format_rate(-100) == "0 B/s");
}

TEST_CASE("format_percent - clamping and decimal control", "[core][formatting]") {
    CHECK(format_percent(0.0) == "0.0%");
    CHECK(format_percent(1.0) == "100.0%");
    CHECK(format_percent(0.4234) == "42.3%");
    CHECK(format_percent(0.4234, 2) == "42.34%");
    CHECK(format_percent(0.4234, 0) == "42%");
    CHECK(format_percent(-0.5) == "0.0%");
    CHECK(format_percent(1.5) == "100.0%");
    CHECK(format_percent(0.5, -3) == "50%"); // negative decimals clamped to 0
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
