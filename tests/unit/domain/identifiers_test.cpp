#include <concepts>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <cmlb/domain/identifiers.hpp>

using cmlb::domain::CallbackQueryId;
using cmlb::domain::ChatId;
using cmlb::domain::FileId;
using cmlb::domain::Gid;
using cmlb::domain::MessageId;
using cmlb::domain::StrongId;
using cmlb::domain::TaskId;
using cmlb::domain::UserId;

// -----------------------------------------------------------------------------
// Compile-time guarantees: distinct tags must produce non-interchangeable types.
// -----------------------------------------------------------------------------
static_assert(!std::equality_comparable_with<ChatId, UserId>,
              "ChatId and UserId must not be interchangeably comparable");
static_assert(!std::equality_comparable_with<ChatId, MessageId>,
              "ChatId and MessageId must not be interchangeably comparable");
static_assert(!std::equality_comparable_with<UserId, MessageId>,
              "UserId and MessageId must not be interchangeably comparable");
static_assert(!std::same_as<ChatId, UserId>,
              "ChatId and UserId must be distinct types");
static_assert(std::is_default_constructible_v<ChatId>);
static_assert(std::is_default_constructible_v<Gid>);
static_assert(std::equality_comparable<ChatId>);
static_assert(std::totally_ordered<ChatId>);

TEST_CASE("StrongId default construction yields zero/empty underlying", "[domain][identifiers]") {
    constexpr ChatId default_chat;
    STATIC_REQUIRE(default_chat.value() == 0);

    const Gid default_gid;
    REQUIRE(default_gid.value().empty());
}

TEST_CASE("StrongId value round-trips the wrapped value", "[domain][identifiers]") {
    constexpr ChatId chat{-1001234567890};
    STATIC_REQUIRE(chat.value() == -1001234567890);

    const Gid gid{std::string{"deadbeefcafef00d"}};
    REQUIRE(gid.value() == "deadbeefcafef00d");

    const TaskId task{std::string{"01890b3c-7f0c-7e0e-bb91-aa8b1a0d3f01"}};
    REQUIRE(task.value() == "01890b3c-7f0c-7e0e-bb91-aa8b1a0d3f01");
}

TEST_CASE("StrongId take() moves the underlying out", "[domain][identifiers]") {
    Gid gid{std::string(64, 'a')};
    auto raw = std::move(gid).take();
    REQUIRE(raw.size() == 64);
    REQUIRE(raw.front() == 'a');
}

TEST_CASE("StrongId equality and ordering follow the underlying", "[domain][identifiers]") {
    constexpr ChatId a{100};
    constexpr ChatId b{100};
    constexpr ChatId c{200};

    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE(a != c);
    STATIC_REQUIRE(a < c);
    STATIC_REQUIRE(c > a);
    STATIC_REQUIRE(a <= b);
    STATIC_REQUIRE(c >= b);

    REQUIRE((a <=> c) == std::strong_ordering::less);
    REQUIRE((c <=> a) == std::strong_ordering::greater);
    REQUIRE((a <=> b) == std::strong_ordering::equal);
}

TEST_CASE("StrongId can be used as an unordered_map key", "[domain][identifiers]") {
    std::unordered_map<ChatId, int> counts;
    counts[ChatId{42}] = 1;
    counts[ChatId{43}] = 2;
    counts[ChatId{42}] += 5;

    REQUIRE(counts.size() == 2);
    REQUIRE(counts[ChatId{42}] == 6);
    REQUIRE(counts[ChatId{43}] == 2);

    std::unordered_set<UserId> users;
    users.insert(UserId{1});
    users.insert(UserId{2});
    users.insert(UserId{1});
    REQUIRE(users.size() == 2);
}

TEST_CASE("StrongId with string underlying hashes correctly", "[domain][identifiers]") {
    std::unordered_map<Gid, int> by_gid;
    by_gid[Gid{std::string{"aaaa"}}] = 1;
    by_gid[Gid{std::string{"bbbb"}}] = 2;
    REQUIRE(by_gid.at(Gid{std::string{"aaaa"}}) == 1);
    REQUIRE(by_gid.size() == 2);
}

TEST_CASE("fmt::formatter prints the underlying value", "[domain][identifiers]") {
    REQUIRE(fmt::format("{}", ChatId{12345}) == "12345");
    REQUIRE(fmt::format("{}", UserId{-7}) == "-7");
    REQUIRE(fmt::format("{}", FileId{3}) == "3");
    REQUIRE(fmt::format("{}", Gid{std::string{"hex"}}) == "hex");
    REQUIRE(fmt::format("chat={}", ChatId{42}) == "chat=42");
    REQUIRE(fmt::format("{}", CallbackQueryId{99}) == "99");
}

TEST_CASE("Distinct StrongIds with the same underlying compile to distinct types",
          "[domain][identifiers]") {
    using A = StrongId<struct LocalA, std::int64_t>;
    using B = StrongId<struct LocalB, std::int64_t>;
    STATIC_REQUIRE(!std::same_as<A, B>);
    STATIC_REQUIRE(!std::equality_comparable_with<A, B>);
}
