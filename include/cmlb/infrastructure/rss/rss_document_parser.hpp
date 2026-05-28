#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cmlb/core/error.hpp>

/// @file rss_document_parser.hpp
/// @brief Lightweight RSS 2.0 / Atom 1.0 feed parser. Uses regex-based
///        extraction (full XML parsing is overkill for the malformed feeds
///        we encounter in practice) and is deliberately permissive.

namespace cmlb::infrastructure::rss {

/// A single feed entry. Field naming is normalised across RSS 2.0 and Atom.
struct RssEntry {
    /// GUID / id. RSS: `<guid>`, Atom: `<id>`. Defaults to the entry's link
    /// when neither is present.
    std::string guid;
    /// Display title.
    std::string title;
    /// Primary link to the entry's web page (RSS: `<link>`, Atom: `<link href=...>`).
    std::string link;
    /// Magnet URI scraped from the description or, less commonly, an enclosure.
    std::optional<std::string> magnet;
    /// URL of an enclosure whose `type` is `application/x-bittorrent`.
    std::optional<std::string> torrent_url;
    /// Description / summary. HTML entities and tags are *not* stripped.
    std::optional<std::string> description;
    /// Publication timestamp parsed from `<pubDate>` (RFC 822) or
    /// `<updated>` / `<published>` (RFC 3339).
    std::optional<std::chrono::system_clock::time_point> published_at;
};

/// The root of a feed document.
struct RssDocument {
    /// Channel / feed title.
    std::string title;
    /// Channel / feed link (homepage URL).
    std::string link;
    /// Optional description.
    std::optional<std::string> description;
    /// Entries in source order.
    std::vector<RssEntry> entries;
};

/// Stateless façade over the regex-based parser.
class RssDocumentParser {
public:
    /// Parses @p xml and returns a populated document. Detects RSS vs Atom
    /// from the root element. Returns `Deserialization` on malformed input.
    [[nodiscard]] static cmlb::core::Result<RssDocument> parse(std::string_view xml);
};

} // namespace cmlb::infrastructure::rss
