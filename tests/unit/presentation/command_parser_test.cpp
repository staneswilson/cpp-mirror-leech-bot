// ---------------------------------------------------------------------------
// command_parser_test.cpp
//
// Unit tests for `cmlb::presentation::CommandParser`. The parser is a pure,
// stateless transformation so the tests are simple assertion suites.
// ---------------------------------------------------------------------------

#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <cmlb/domain/identifiers.hpp>
#include <cmlb/presentation/command_parser.hpp>

using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::UserId;
using cmlb::presentation::CommandParser;
using cmlb::presentation::CommandRequest;

namespace {

constexpr auto kSender = UserId{42};
constexpr auto kChat   = ChatId{-1001};
constexpr auto kMsg    = MessageId{17};

[[nodiscard]] std::optional<CommandRequest> parse(std::string_view text) {
    return CommandParser::parse(text, kSender, kChat, kMsg);
}

}  // namespace

TEST_CASE("CommandParser rejects non-command text", "[presentation][parser]") {
    CHECK_FALSE(parse("").has_value());
    CHECK_FALSE(parse("hello world").has_value());
    CHECK_FALSE(parse("   ").has_value());
    CHECK_FALSE(parse("not a /command").has_value());
}

TEST_CASE("CommandParser rejects a bare slash", "[presentation][parser]") {
    CHECK_FALSE(parse("/").has_value());
    CHECK_FALSE(parse("  /  ").has_value());
}

TEST_CASE("CommandParser parses commands without arguments",
          "[presentation][parser]") {
    const auto result = parse("/mirror");
    REQUIRE(result.has_value());
    CHECK(result->command   == "mirror");
    CHECK(result->arguments == "");
    CHECK(result->full_text == "/mirror");
    CHECK(result->sender    == kSender);
    CHECK(result->chat      == kChat);
    CHECK(result->source_message == kMsg);
}

TEST_CASE("CommandParser parses commands with arguments",
          "[presentation][parser]") {
    const auto result = parse("/mirror https://example.com/file.iso");
    REQUIRE(result.has_value());
    CHECK(result->command   == "mirror");
    CHECK(result->arguments == "https://example.com/file.iso");
}

TEST_CASE("CommandParser parses short aliases", "[presentation][parser]") {
    const auto result = parse("/m https://example.com/file.iso");
    REQUIRE(result.has_value());
    CHECK(result->command   == "m");
    CHECK(result->arguments == "https://example.com/file.iso");
}

TEST_CASE("CommandParser strips '@BotUsername' suffix",
          "[presentation][parser]") {
    const auto result = parse("/mirror@CmlbBot https://example.com");
    REQUIRE(result.has_value());
    CHECK(result->command   == "mirror");
    CHECK(result->arguments == "https://example.com");
}

TEST_CASE("CommandParser strips '@BotUsername' suffix when no arguments",
          "[presentation][parser]") {
    const auto result = parse("/status@CmlbBot");
    REQUIRE(result.has_value());
    CHECK(result->command   == "status");
    CHECK(result->arguments == "");
}

TEST_CASE("CommandParser trims surrounding whitespace",
          "[presentation][parser]") {
    const auto result = parse("   /mirror    http://x.com    ");
    REQUIRE(result.has_value());
    CHECK(result->command   == "mirror");
    CHECK(result->arguments == "http://x.com");
}

TEST_CASE("CommandParser preserves internal whitespace in arguments",
          "[presentation][parser]") {
    const auto result = parse("/rss add  https://feed.example.com/rss");
    REQUIRE(result.has_value());
    CHECK(result->command   == "rss");
    CHECK(result->arguments == "add  https://feed.example.com/rss");
}

TEST_CASE("CommandParser lower-cases the command name",
          "[presentation][parser]") {
    const auto result = parse("/MIRROR url");
    REQUIRE(result.has_value());
    CHECK(result->command   == "mirror");
    CHECK(result->arguments == "url");
}

TEST_CASE("CommandParser preserves full_text verbatim",
          "[presentation][parser]") {
    const std::string original = "  /Mirror@Bot  URL  ";
    const auto result = parse(original);
    REQUIRE(result.has_value());
    CHECK(result->full_text == original);
}

TEST_CASE("CommandParser returns nullopt when only '@' precedes whitespace",
          "[presentation][parser]") {
    // `/@bot` collapses to an empty command name after stripping the bot
    // suffix; we treat that as a non-command.
    CHECK_FALSE(parse("/@CmlbBot").has_value());
}

TEST_CASE("CommandParser keeps tab-separated arguments",
          "[presentation][parser]") {
    const auto result = parse("/leech\thttps://example.com");
    REQUIRE(result.has_value());
    CHECK(result->command   == "leech");
    CHECK(result->arguments == "https://example.com");
}
