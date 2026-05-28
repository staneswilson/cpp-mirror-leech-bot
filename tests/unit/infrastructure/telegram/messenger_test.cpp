// ---------------------------------------------------------------------------
// messenger_test.cpp
//
// Unit tests for the pure (gateway-independent) parts of `Messenger`.
//
// The TDLib-touching code paths cannot be exercised here without standing up
// a real TDLib client, so we focus on the static helper that builds the
// refresh+close button row.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <cmlb/infrastructure/telegram/messenger.hpp>

using cmlb::infrastructure::telegram::InlineKeyboard;
using cmlb::infrastructure::telegram::Messenger;

TEST_CASE("Messenger::refresh_close_row produces one row of two buttons",
          "[telegram][messenger]") {
    const auto kb = Messenger::refresh_close_row("task-42");

    REQUIRE(kb.size() == 1);
    REQUIRE(kb.front().size() == 2);
}

TEST_CASE("Messenger::refresh_close_row places refresh first with provided data",
          "[telegram][messenger]") {
    const auto kb = Messenger::refresh_close_row("task-42");

    const auto& refresh = kb.front()[0];
    CHECK(refresh.label         == "Refresh");
    CHECK(refresh.callback_data == "task-42");
}

TEST_CASE("Messenger::refresh_close_row pins the close button data to \"close\"",
          "[telegram][messenger]") {
    const auto kb = Messenger::refresh_close_row("anything");

    const auto& close_btn = kb.front()[1];
    CHECK(close_btn.label         == "Close");
    CHECK(close_btn.callback_data == "close");
}

TEST_CASE("Messenger::refresh_close_row callback data is propagated verbatim",
          "[telegram][messenger]") {
    SECTION("alphanumeric token") {
        const auto kb = Messenger::refresh_close_row("abc123");
        CHECK(kb.front()[0].callback_data == "abc123");
    }
    SECTION("token with separator") {
        const auto kb = Messenger::refresh_close_row("status:task/42");
        CHECK(kb.front()[0].callback_data == "status:task/42");
    }
    SECTION("empty token") {
        const auto kb = Messenger::refresh_close_row("");
        CHECK(kb.front()[0].callback_data.empty());
        CHECK(kb.front()[1].callback_data == "close");
    }
}
