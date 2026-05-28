#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <cmlb/core/error.hpp>

/// @file byte_size.hpp
/// @brief Strong type for byte counts with safe arithmetic and unit literals.

namespace cmlb::domain {

/// A non-negative byte count. Construction goes through the @ref make factory
/// (or @ref from_unchecked for trusted constants/literals) to preserve the
/// invariant: `bytes() >= 0`.
class ByteSize {
public:
    /// @name Unit constants (powers of two, IEC binary prefixes).
    /// @{
    static constexpr std::int64_t BYTE = 1;
    static constexpr std::int64_t KIB = 1024;
    static constexpr std::int64_t MIB = 1024LL * 1024;
    static constexpr std::int64_t GIB = 1024LL * 1024 * 1024;
    static constexpr std::int64_t TIB = 1024LL * 1024 * 1024 * 1024;
    /// @}

    /// Telegram bot API hard cap for a single file part (2 GB).
    static constexpr std::int64_t TELEGRAM_SPLIT_MAX = 2'000'000'000;

    constexpr ByteSize() noexcept = default;

    /// Construct from a known-valid (non-negative) byte count. Use only when
    /// the caller can statically prove the value is non-negative (e.g.,
    /// user-defined literals, unit constants).
    [[nodiscard]] static constexpr ByteSize from_unchecked(std::int64_t bytes) noexcept {
        return ByteSize{bytes, unchecked_tag{}};
    }

    /// Validated factory: returns InvalidArgument if @p bytes is negative.
    [[nodiscard]] static constexpr cmlb::core::Result<ByteSize> make(
        std::int64_t bytes, std::source_location loc = std::source_location::current()) {
        if (bytes < 0) {
            return cmlb::core::error(
                cmlb::core::ErrorCode::InvalidArgument, "ByteSize must be non-negative", loc);
        }
        return ByteSize{bytes, unchecked_tag{}};
    }

    [[nodiscard]] constexpr std::int64_t bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr auto operator<=>(const ByteSize&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const ByteSize&) const noexcept = default;

    /// Saturating addition: clamps at int64 max rather than wrapping.
    [[nodiscard]] constexpr ByteSize operator+(const ByteSize& other) const noexcept {
        constexpr auto k_max = std::numeric_limits<std::int64_t>::max();
        if (other.bytes_ > k_max - bytes_) {
            return ByteSize{k_max, unchecked_tag{}};
        }
        return ByteSize{bytes_ + other.bytes_, unchecked_tag{}};
    }

    constexpr ByteSize& operator+=(const ByteSize& other) noexcept {
        *this = *this + other;
        return *this;
    }

    /// Saturating multiplication by a non-negative integral scalar.
    template <typename Integral>
        requires std::is_integral_v<Integral>
    [[nodiscard]] constexpr ByteSize operator*(Integral factor) const noexcept {
        if (factor <= Integral{0} || bytes_ == 0) {
            return ByteSize{0, unchecked_tag{}};
        }
        constexpr auto k_max = std::numeric_limits<std::int64_t>::max();
        const auto f64 = static_cast<std::int64_t>(factor);
        if (bytes_ > k_max / f64) {
            return ByteSize{k_max, unchecked_tag{}};
        }
        return ByteSize{bytes_ * f64, unchecked_tag{}};
    }

private:
    struct unchecked_tag {};

    constexpr ByteSize(std::int64_t bytes, unchecked_tag) noexcept : bytes_{bytes} {
    }

    std::int64_t bytes_{0};
};

/// Free function: scalar * ByteSize (commutative form of operator*).
template <typename Integral>
    requires std::is_integral_v<Integral>
[[nodiscard]] constexpr ByteSize operator*(Integral factor, const ByteSize& size) noexcept {
    return size * factor;
}

// --------------------------------------------------------------------------
// User-defined literals — `inline namespace literals` so users can write
// `using namespace cmlb::domain::literals;` selectively.
// --------------------------------------------------------------------------
inline namespace literals {

[[nodiscard]] consteval ByteSize operator""_b(unsigned long long value) noexcept {
    return ByteSize::from_unchecked(static_cast<std::int64_t>(value));
}

[[nodiscard]] consteval ByteSize operator""_KiB(unsigned long long value) noexcept {
    return ByteSize::from_unchecked(static_cast<std::int64_t>(value) * ByteSize::KIB);
}

[[nodiscard]] consteval ByteSize operator""_MiB(unsigned long long value) noexcept {
    return ByteSize::from_unchecked(static_cast<std::int64_t>(value) * ByteSize::MIB);
}

[[nodiscard]] consteval ByteSize operator""_GiB(unsigned long long value) noexcept {
    return ByteSize::from_unchecked(static_cast<std::int64_t>(value) * ByteSize::GIB);
}

[[nodiscard]] consteval ByteSize operator""_TiB(unsigned long long value) noexcept {
    return ByteSize::from_unchecked(static_cast<std::int64_t>(value) * ByteSize::TIB);
}

} // namespace literals

} // namespace cmlb::domain
