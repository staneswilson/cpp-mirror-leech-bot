#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

#include <cmlb/domain/identifiers.hpp>

/// @file task_id_generator.hpp
/// @brief Local TaskId factory shared by the application layer.
///
/// Produces a 32-character lowercase hex string seeded from `std::random_device`
/// (16 bytes of entropy). Intentionally avoids pulling in a UUID dependency.

namespace cmlb::application::detail {

[[nodiscard]] inline cmlb::domain::TaskId make_task_id() {
    std::random_device rd;
    // Two 64-bit draws → 16 bytes.
    const std::uint64_t hi = (static_cast<std::uint64_t>(rd()) << 32)
                             | static_cast<std::uint64_t>(rd());
    const std::uint64_t lo = (static_cast<std::uint64_t>(rd()) << 32)
                             | static_cast<std::uint64_t>(rd());

    static constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    std::string out;
    out.resize(32);
    auto pour = [&](std::uint64_t value, std::size_t offset) {
        for (std::size_t i = 0; i < 16; ++i) {
            out[offset + 15 - i] = kHex[value & 0xF];
            value >>= 4;
        }
    };
    pour(hi, 0);
    pour(lo, 16);
    return cmlb::domain::TaskId{std::move(out)};
}

}  // namespace cmlb::application::detail
