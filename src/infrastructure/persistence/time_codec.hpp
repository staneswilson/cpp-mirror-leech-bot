#pragma once

// Internal helper header — translation-unit private to the persistence
// library. Provides ISO 8601 codec helpers for the repositories. Not
// installed; not part of the public API surface.

#include <chrono>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <cmlb/core/error.hpp>

namespace cmlb::infrastructure::persistence::detail {

using SysTime = std::chrono::system_clock::time_point;

[[nodiscard]] inline std::string to_iso8601(SysTime tp) {
    using namespace std::chrono;
    const auto truncated = floor<milliseconds>(tp);
    return fmt::format("{:%Y-%m-%dT%H:%M:%S}Z", truncated);
}

[[nodiscard]] inline core::Result<SysTime> parse_iso8601(std::string_view s) {
    if (s.size() < 19) {
        return core::error(core::ErrorCode::Deserialization,
                           "ISO8601 timestamp too short: " + std::string{s});
    }
    int year = 0;
    int mon = 0;
    int day = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    int ms = 0;

    auto take_int = [&](std::size_t off, std::size_t len, int& out) -> bool {
        if (off + len > s.size())
            return false;
        int v = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const char c = s[off + i];
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };

    if (!take_int(0, 4, year) || s[4] != '-' || !take_int(5, 2, mon) || s[7] != '-'
        || !take_int(8, 2, day) || s[10] != 'T' || !take_int(11, 2, hh) || s[13] != ':'
        || !take_int(14, 2, mm) || s[16] != ':' || !take_int(17, 2, ss)) {
        return core::error(core::ErrorCode::Deserialization,
                           "Malformed ISO8601 timestamp: " + std::string{s});
    }

    std::size_t cursor = 19;
    if (cursor < s.size() && s[cursor] == '.') {
        std::size_t digits = 0;
        int frac = 0;
        ++cursor;
        while (cursor < s.size() && digits < 3 && s[cursor] >= '0' && s[cursor] <= '9') {
            frac = frac * 10 + (s[cursor] - '0');
            ++cursor;
            ++digits;
        }
        while (digits < 3) {
            frac *= 10;
            ++digits;
        }
        while (cursor < s.size() && s[cursor] >= '0' && s[cursor] <= '9') {
            ++cursor;
        }
        ms = frac;
    }

    using namespace std::chrono;
    const auto ymd = year_month_day{std::chrono::year{year},
                                    std::chrono::month{static_cast<unsigned>(mon)},
                                    std::chrono::day{static_cast<unsigned>(day)}};
    if (!ymd.ok()) {
        return core::error(core::ErrorCode::Deserialization,
                           "Out-of-range Y/M/D in timestamp: " + std::string{s});
    }
    const sys_days day_point = sys_days{ymd};
    return SysTime{day_point + hours{hh} + minutes{mm} + seconds{ss} + milliseconds{ms}};
}

} // namespace cmlb::infrastructure::persistence::detail
