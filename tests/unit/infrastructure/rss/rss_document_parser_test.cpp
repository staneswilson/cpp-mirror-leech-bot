#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/rss/rss_document_parser.hpp>

using Catch::Matchers::ContainsSubstring;
using cmlb::core::ErrorCode;
using cmlb::infrastructure::rss::RssDocumentParser;

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

constexpr std::string_view kRssWithMagnetInDescription = R"(<?xml version="1.0"?>
<rss version="2.0">
  <channel>
    <title>Test Tracker</title>
    <link>https://example.org/</link>
    <description>Latest torrents</description>
    <item>
      <title>Ubuntu 24.04 LTS Desktop amd64</title>
      <link>https://example.org/torrents/1</link>
      <guid>https://example.org/torrents/1</guid>
      <description><![CDATA[See <a href="magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567&dn=Ubuntu">this magnet</a>]]></description>
      <pubDate>Wed, 02 Oct 2024 13:00:00 GMT</pubDate>
    </item>
    <item>
      <title>Debian 12 netinst</title>
      <link>https://example.org/torrents/2</link>
      <guid>https://example.org/torrents/2</guid>
      <enclosure url="https://example.org/torrents/2.torrent" length="42" type="application/x-bittorrent"/>
    </item>
  </channel>
</rss>)";

constexpr std::string_view kAtomFeed = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>Atomic News</title>
  <link rel="self" href="https://example.org/atom.xml"/>
  <link rel="alternate" href="https://example.org/"/>
  <subtitle>Sample subtitle</subtitle>
  <entry>
    <title>Release 1.2.3</title>
    <id>urn:uuid:11111111-1111-1111-1111-111111111111</id>
    <link rel="alternate" href="https://example.org/posts/123"/>
    <summary>Lots of changes. magnet:?xt=urn:btih:CAFEBABECAFEBABECAFEBABECAFEBABECAFEBABE&dn=Release</summary>
    <published>2024-10-02T13:00:00Z</published>
  </entry>
</feed>)";

constexpr std::string_view kMalformed = R"(<<<not really xml>>>)";

} // namespace

TEST_CASE("parse RSS 2.0 with channel metadata and items", "[infra][rss][parser]") {
    auto result = RssDocumentParser::parse(kRssWithMagnetInDescription);
    REQUIRE(result);

    const auto& doc = *result;
    CHECK(doc.title == "Test Tracker");
    CHECK(doc.link == "https://example.org/");
    REQUIRE(doc.description);
    CHECK(*doc.description == "Latest torrents");

    REQUIRE(doc.entries.size() == 2);
    const auto& first = doc.entries[0];
    CHECK(first.title == "Ubuntu 24.04 LTS Desktop amd64");
    CHECK(first.link == "https://example.org/torrents/1");
    CHECK(first.guid == "https://example.org/torrents/1");
    REQUIRE(first.magnet);
    CHECK_THAT(*first.magnet, ContainsSubstring("magnet:?xt=urn:btih:"));
    CHECK_THAT(*first.magnet, ContainsSubstring("0123456789ABCDEF"));
    REQUIRE(first.published_at);

    const auto& second = doc.entries[1];
    CHECK(second.title == "Debian 12 netinst");
    REQUIRE(second.torrent_url);
    CHECK(*second.torrent_url == "https://example.org/torrents/2.torrent");
}

TEST_CASE("parse Atom 1.0 with alternate link and magnet in summary", "[infra][rss][parser]") {
    auto result = RssDocumentParser::parse(kAtomFeed);
    REQUIRE(result);

    const auto& doc = *result;
    CHECK(doc.title == "Atomic News");
    CHECK(doc.link == "https://example.org/");
    REQUIRE(doc.description);
    CHECK(*doc.description == "Sample subtitle");

    REQUIRE(doc.entries.size() == 1);
    const auto& entry = doc.entries[0];
    CHECK(entry.title == "Release 1.2.3");
    CHECK(entry.guid == "urn:uuid:11111111-1111-1111-1111-111111111111");
    CHECK(entry.link == "https://example.org/posts/123");
    REQUIRE(entry.magnet);
    CHECK_THAT(*entry.magnet, ContainsSubstring("CAFEBABE"));
    REQUIRE(entry.published_at);
}

TEST_CASE("magnet extraction is anchored and does not match arbitrary text",
          "[infra][rss][parser]") {
    constexpr std::string_view kFeed = R"(<?xml version="1.0"?>
<rss version="2.0">
  <channel><title>x</title><link>https://x</link>
    <item>
      <title>has magnet</title>
      <guid>g1</guid>
      <link>https://x/1</link>
      <description>not-a-magnet: definitely not magnet:?xt=urn:btih:DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEF&tr=udp://t</description>
    </item>
    <item>
      <title>no magnet</title>
      <guid>g2</guid>
      <link>https://x/2</link>
      <description>plain text without any magnet URI in it</description>
    </item>
  </channel>
</rss>)";

    auto result = RssDocumentParser::parse(kFeed);
    REQUIRE(result);
    REQUIRE(result->entries.size() == 2);

    REQUIRE(result->entries[0].magnet);
    CHECK_THAT(*result->entries[0].magnet, ContainsSubstring("DEADBEEF"));
    CHECK_FALSE(result->entries[1].magnet);
}

TEST_CASE("malformed XML returns a Deserialization error", "[infra][rss][parser]") {
    auto result = RssDocumentParser::parse(kMalformed);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Deserialization);
}

TEST_CASE("empty input returns a Deserialization error", "[infra][rss][parser]") {
    auto result = RssDocumentParser::parse("");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Deserialization);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
