#include <array>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/identifiers.hpp>

using cmlb::domain::Authority;
using cmlb::domain::ChatId;
using cmlb::domain::Permission;
using cmlb::domain::UserId;
using cmlb::domain::from_string;
using cmlb::domain::to_string;

namespace {

constexpr UserId OWNER{1};
constexpr UserId SUDO_A{2};
constexpr UserId SUDO_B{3};
constexpr UserId REGULAR{4};
constexpr UserId STRANGER{5};

constexpr ChatId AUTH_CHAT_A{-100100};
constexpr ChatId AUTH_CHAT_B{-100200};
constexpr ChatId UNAUTH_CHAT{-100999};

[[nodiscard]] Authority make_authority() {
    static const std::array<UserId, 2> sudo{SUDO_A, SUDO_B};
    static const std::array<ChatId, 2> chats{AUTH_CHAT_A, AUTH_CHAT_B};
    return Authority{OWNER,
                     std::span<const UserId>{sudo},
                     std::span<const ChatId>{chats}};
}

}  // namespace

TEST_CASE("to_string and from_string round-trip permissions", "[domain][authority]") {
    REQUIRE(to_string(Permission::Anyone) == "anyone");
    REQUIRE(to_string(Permission::User)   == "user");
    REQUIRE(to_string(Permission::Admin)  == "admin");
    REQUIRE(to_string(Permission::Owner)  == "owner");

    REQUIRE(from_string("anyone") == Permission::Anyone);
    REQUIRE(from_string("USER")   == Permission::User);
    REQUIRE(from_string("Admin")  == Permission::Admin);
    REQUIRE(from_string("sudo")   == Permission::Admin);
    REQUIRE(from_string("OWNER")  == Permission::Owner);
    REQUIRE(from_string("nope")   == Permission::Anyone);
    REQUIRE(from_string("")       == Permission::Anyone);
}

TEST_CASE("Authority::of follows the truth table", "[domain][authority]") {
    const auto authority = make_authority();

    SECTION("owner from any chat is Owner") {
        REQUIRE(authority.of(OWNER, AUTH_CHAT_A)  == Permission::Owner);
        REQUIRE(authority.of(OWNER, UNAUTH_CHAT)  == Permission::Owner);
    }

    SECTION("sudo user from any chat is Admin") {
        REQUIRE(authority.of(SUDO_A, AUTH_CHAT_A) == Permission::Admin);
        REQUIRE(authority.of(SUDO_B, UNAUTH_CHAT) == Permission::Admin);
    }

    SECTION("regular user inside authorized chat is User") {
        REQUIRE(authority.of(REGULAR,  AUTH_CHAT_A) == Permission::User);
        REQUIRE(authority.of(STRANGER, AUTH_CHAT_B) == Permission::User);
    }

    SECTION("regular user outside authorized chats is Anyone") {
        REQUIRE(authority.of(REGULAR,  UNAUTH_CHAT) == Permission::Anyone);
        REQUIRE(authority.of(STRANGER, UNAUTH_CHAT) == Permission::Anyone);
    }
}

TEST_CASE("Authority::can_run honours the hierarchy", "[domain][authority]") {
    const auto authority = make_authority();

    SECTION("owner can run anything") {
        REQUIRE(authority.can_run(OWNER, UNAUTH_CHAT, Permission::Anyone));
        REQUIRE(authority.can_run(OWNER, UNAUTH_CHAT, Permission::User));
        REQUIRE(authority.can_run(OWNER, UNAUTH_CHAT, Permission::Admin));
        REQUIRE(authority.can_run(OWNER, UNAUTH_CHAT, Permission::Owner));
    }

    SECTION("sudo can run up to Admin but not Owner-only") {
        REQUIRE(authority.can_run(SUDO_A, UNAUTH_CHAT, Permission::Anyone));
        REQUIRE(authority.can_run(SUDO_A, UNAUTH_CHAT, Permission::User));
        REQUIRE(authority.can_run(SUDO_A, UNAUTH_CHAT, Permission::Admin));
        REQUIRE_FALSE(authority.can_run(SUDO_A, UNAUTH_CHAT, Permission::Owner));
    }

    SECTION("user in authorised chat can run up to User") {
        REQUIRE(authority.can_run(REGULAR, AUTH_CHAT_A, Permission::Anyone));
        REQUIRE(authority.can_run(REGULAR, AUTH_CHAT_A, Permission::User));
        REQUIRE_FALSE(authority.can_run(REGULAR, AUTH_CHAT_A, Permission::Admin));
        REQUIRE_FALSE(authority.can_run(REGULAR, AUTH_CHAT_A, Permission::Owner));
    }

    SECTION("stranger in unauthorised chat can only run Anyone-level commands") {
        REQUIRE(authority.can_run(STRANGER, UNAUTH_CHAT, Permission::Anyone));
        REQUIRE_FALSE(authority.can_run(STRANGER, UNAUTH_CHAT, Permission::User));
        REQUIRE_FALSE(authority.can_run(STRANGER, UNAUTH_CHAT, Permission::Admin));
        REQUIRE_FALSE(authority.can_run(STRANGER, UNAUTH_CHAT, Permission::Owner));
    }
}

TEST_CASE("Authority handles empty sudo/chat lists", "[domain][authority]") {
    const Authority authority{OWNER, std::span<const UserId>{}, std::span<const ChatId>{}};
    REQUIRE(authority.of(OWNER,    AUTH_CHAT_A) == Permission::Owner);
    REQUIRE(authority.of(REGULAR,  AUTH_CHAT_A) == Permission::Anyone);
    REQUIRE(authority.of(STRANGER, UNAUTH_CHAT) == Permission::Anyone);
}

TEST_CASE("Authority owner takes precedence over sudo membership", "[domain][authority]") {
    const std::array<UserId, 1> sudo{OWNER};  // pathological: owner also listed as sudo
    const std::array<ChatId, 0> chats{};
    const Authority authority{OWNER,
                              std::span<const UserId>{sudo},
                              std::span<const ChatId>{chats}};
    REQUIRE(authority.of(OWNER, UNAUTH_CHAT) == Permission::Owner);
}
