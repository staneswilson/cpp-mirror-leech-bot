#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

/// @file identifiers.hpp
/// @brief Phantom-typed identifier wrappers preventing cross-domain confusion
///        (e.g. a ChatId cannot be accidentally compared with a UserId).

namespace cmlb::domain {

/// Strong identifier template. The @p Tag template parameter makes each
/// instantiation a distinct type even when the @p Underlying representation
/// matches. This prevents implicit interchange of conceptually different IDs.
template <typename Tag, typename Underlying>
class StrongId {
public:
    using value_type = Underlying;

    constexpr StrongId() = default;

    constexpr explicit StrongId(Underlying value) noexcept(
        std::is_nothrow_move_constructible_v<Underlying>)
        : value_{std::move(value)} {
    }

    [[nodiscard]] constexpr const Underlying& value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr Underlying&& take() && noexcept {
        return std::move(value_);
    }

    [[nodiscard]] constexpr auto operator<=>(const StrongId&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const StrongId&) const noexcept = default;

private:
    Underlying value_{};
};

/// Telegram chat identifier (1:1 chats are positive, groups are negative).
using ChatId = StrongId<struct ChatIdTag, std::int64_t>;

/// Telegram user identifier (always positive).
using UserId = StrongId<struct UserIdTag, std::int64_t>;

/// Telegram message identifier within a chat.
using MessageId = StrongId<struct MessageIdTag, std::int64_t>;

/// aria2 download identifier (16-character lowercase hex string).
using Gid = StrongId<struct GidTag, std::string>;

/// Internal task identifier (UUIDv7 textual representation).
using TaskId = StrongId<struct TaskIdTag, std::string>;

/// TDLib local file identifier (process-local, monotonic).
using FileId = StrongId<struct FileIdTag, std::int32_t>;

/// Telegram callback query identifier delivered alongside inline button presses.
using CallbackQueryId = StrongId<struct CallbackQueryIdTag, std::int64_t>;

} // namespace cmlb::domain

// --------------------------------------------------------------------------
// std::hash specialization — chained through hash<Underlying> so any StrongId
// can serve as a key in std::unordered_map / std::unordered_set.
// --------------------------------------------------------------------------
namespace std {

template <typename Tag, typename Underlying>
struct hash<::cmlb::domain::StrongId<Tag, Underlying>> {
    [[nodiscard]] std::size_t operator()(const ::cmlb::domain::StrongId<Tag, Underlying>& id) const
        noexcept(noexcept(std::hash<Underlying>{}(id.value()))) {
        return std::hash<Underlying>{}(id.value());
    }
};

} // namespace std

// --------------------------------------------------------------------------
// fmt::formatter specialization — forwards to the underlying value's formatter
// so logger calls like `Logger::info("chat={}", chat_id)` work transparently.
// --------------------------------------------------------------------------
namespace fmt {

template <typename Tag, typename Underlying, typename CharT>
struct formatter<::cmlb::domain::StrongId<Tag, Underlying>, CharT> : formatter<Underlying, CharT> {
    template <typename FormatContext>
    auto format(const ::cmlb::domain::StrongId<Tag, Underlying>& id, FormatContext& ctx) const {
        return formatter<Underlying, CharT>::format(id.value(), ctx);
    }
};

} // namespace fmt
