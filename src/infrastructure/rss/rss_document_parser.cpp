#include <cmlb/infrastructure/rss/rss_document_parser.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cmlb/core/error.hpp>

namespace cmlb::infrastructure::rss {

namespace {

using cmlb::core::ErrorCode;
using cmlb::core::Result;
using cmlb::core::error;

// ---------------------------------------------------------------------------
// Compiled regex constants. ECMAScript flavour, case-insensitive where the
// tag name is being matched.
// ---------------------------------------------------------------------------

const std::regex& rss_root_regex() {
    static const std::regex r{R"(<rss\b)", std::regex::icase};
    return r;
}

const std::regex& atom_root_regex() {
    static const std::regex r{R"(<feed\b)", std::regex::icase};
    return r;
}

const std::regex& item_regex() {
    static const std::regex r{R"(<item\b[^>]*>([\s\S]*?)</item>)", std::regex::icase};
    return r;
}

const std::regex& atom_entry_regex() {
    static const std::regex r{R"(<entry\b[^>]*>([\s\S]*?)</entry>)", std::regex::icase};
    return r;
}

const std::regex& magnet_regex() {
    static const std::regex r{R"(magnet:\?xt=urn:btih:[A-Fa-f0-9]+[^"<\s]*)"};
    return r;
}

/// Returns the inner text of the first `<tag>...</tag>` match in @p text.
/// CDATA wrappers (`<![CDATA[ ... ]]>`) are unwrapped.
[[nodiscard]] std::optional<std::string> first_tag_text(std::string_view text,
                                                        std::string_view tag) {
    const std::string pattern = "<" + std::string{tag}
                                + R"(\b[^>]*>([\s\S]*?)</)" + std::string{tag} + ">";
    std::regex r{pattern, std::regex::icase};
    std::cmatch m;
    if (!std::regex_search(text.data(), text.data() + text.size(), m, r)) {
        return std::nullopt;
    }
    std::string body = m[1].str();
    // Strip leading/trailing whitespace.
    auto first = body.find_first_not_of(" \t\r\n");
    auto last  = body.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    body = body.substr(first, last - first + 1);

    // Unwrap a single enclosing CDATA section.
    constexpr std::string_view kCdataOpen  = "<![CDATA[";
    constexpr std::string_view kCdataClose = "]]>";
    if (body.starts_with(kCdataOpen) && body.ends_with(kCdataClose)) {
        body = body.substr(kCdataOpen.size(),
                           body.size() - kCdataOpen.size() - kCdataClose.size());
    }
    return body;
}

/// Returns the value of an attribute `attr` within the first tag matching
/// `tag` in @p text. e.g. `attr_of_tag(item, "enclosure", "url")`.
[[nodiscard]] std::optional<std::string> attr_of_tag(std::string_view text,
                                                     std::string_view tag,
                                                     std::string_view attr) {
    const std::string pattern = "<" + std::string{tag} + R"(\b([^>]*)\/?>)";
    std::regex tag_r{pattern, std::regex::icase};
    std::cmatch m;
    if (!std::regex_search(text.data(), text.data() + text.size(), m, tag_r)) {
        return std::nullopt;
    }
    const std::string attrs = m[1].str();
    const std::string attr_pat = std::string{attr} + R"_(\s*=\s*"([^"]*)")_";
    std::regex attr_r{attr_pat, std::regex::icase};
    std::smatch am;
    if (!std::regex_search(attrs, am, attr_r)) {
        // Try single-quoted form.
        const std::string single_pat = std::string{attr} + R"(\s*=\s*'([^']*)')";
        std::regex single_r{single_pat, std::regex::icase};
        if (!std::regex_search(attrs, am, single_r)) {
            return std::nullopt;
        }
    }
    return am[1].str();
}

/// Iterates every `<tag ...>` in @p text and yields it via @p sink as a raw
/// attribute string. Used to scan multiple `<link>` elements in Atom feeds
/// for the "alternate" relation.
template <typename Sink>
void for_each_tag(std::string_view text, std::string_view tag, Sink&& sink) {
    const std::string pattern = "<" + std::string{tag} + R"(\b([^>]*)\/?>)";
    std::regex r{pattern, std::regex::icase};
    auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), r);
    auto end   = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
        sink((*it)[1].str());
    }
}

[[nodiscard]] std::optional<std::string> extract_magnet(std::string_view text) {
    std::cmatch m;
    if (std::regex_search(text.data(), text.data() + text.size(), m, magnet_regex())) {
        return m[0].str();
    }
    return std::nullopt;
}

/// Parses RFC 822 dates (`Wed, 02 Oct 2002 13:00:00 GMT`) — the RSS 2.0
/// canonical form. Returns nullopt on failure.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
parse_rfc822(std::string_view text) {
    std::tm tm{};
    std::istringstream is{std::string{text}};
    is >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");
    if (is.fail()) {
        // Try the variant without weekday.
        std::tm tm2{};
        std::istringstream is2{std::string{text}};
        is2 >> std::get_time(&tm2, "%d %b %Y %H:%M:%S");
        if (is2.fail()) return std::nullopt;
        tm = tm2;
    }
#ifdef _WIN32
    const auto t = _mkgmtime(&tm);
#else
    const auto t = timegm(&tm);
#endif
    if (t == -1) return std::nullopt;
    return std::chrono::system_clock::from_time_t(t);
}

/// Parses RFC 3339 / ISO 8601 timestamps used by Atom feeds.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
parse_rfc3339(std::string_view text) {
    std::tm tm{};
    std::istringstream is{std::string{text}};
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (is.fail()) return std::nullopt;
#ifdef _WIN32
    const auto t = _mkgmtime(&tm);
#else
    const auto t = timegm(&tm);
#endif
    if (t == -1) return std::nullopt;
    return std::chrono::system_clock::from_time_t(t);
}

// ---------------------------------------------------------------------------
// RSS 2.0 parsing
// ---------------------------------------------------------------------------

RssEntry parse_rss_item(std::string_view body) {
    RssEntry entry;
    if (auto v = first_tag_text(body, "title"))       entry.title = std::move(*v);
    if (auto v = first_tag_text(body, "link"))        entry.link  = std::move(*v);
    if (auto v = first_tag_text(body, "guid"))        entry.guid  = std::move(*v);
    if (auto v = first_tag_text(body, "description")) entry.description = std::move(*v);

    if (entry.guid.empty()) entry.guid = entry.link;

    // Enclosure with bittorrent type → torrent_url.
    if (auto enclosure_type = attr_of_tag(body, "enclosure", "type")) {
        if (*enclosure_type == "application/x-bittorrent") {
            entry.torrent_url = attr_of_tag(body, "enclosure", "url");
        }
    }

    // Magnet: prefer description, fall back to whole body.
    if (entry.description) {
        if (auto magnet = extract_magnet(*entry.description)) {
            entry.magnet = std::move(*magnet);
        }
    }
    if (!entry.magnet) {
        if (auto magnet = extract_magnet(body)) {
            entry.magnet = std::move(*magnet);
        }
    }

    if (auto pub = first_tag_text(body, "pubDate")) {
        entry.published_at = parse_rfc822(*pub);
    }
    return entry;
}

Result<RssDocument> parse_rss(std::string_view xml) {
    RssDocument doc;
    // Channel-level metadata: take the first <title>/<link>/<description>
    // we find *before* the first <item>, so item titles don't shadow the
    // channel title.
    const auto first_item = std::cregex_iterator(xml.data(), xml.data() + xml.size(),
                                                 item_regex());
    std::string_view channel_view = xml;
    if (first_item != std::cregex_iterator()) {
        const auto offset = static_cast<std::size_t>((*first_item).position(0));
        channel_view = xml.substr(0, offset);
    }
    if (auto v = first_tag_text(channel_view, "title")) doc.title = std::move(*v);
    if (auto v = first_tag_text(channel_view, "link"))  doc.link  = std::move(*v);
    if (auto v = first_tag_text(channel_view, "description")) {
        doc.description = std::move(*v);
    }

    auto begin = std::cregex_iterator(xml.data(), xml.data() + xml.size(), item_regex());
    auto end   = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
        doc.entries.emplace_back(parse_rss_item((*it)[1].str()));
    }
    return doc;
}

// ---------------------------------------------------------------------------
// Atom parsing
// ---------------------------------------------------------------------------

RssEntry parse_atom_entry(std::string_view body) {
    RssEntry entry;
    if (auto v = first_tag_text(body, "title")) entry.title = std::move(*v);
    if (auto v = first_tag_text(body, "id"))    entry.guid  = std::move(*v);
    if (auto v = first_tag_text(body, "summary"))     entry.description = std::move(*v);
    if (!entry.description) {
        if (auto v = first_tag_text(body, "content")) entry.description = std::move(*v);
    }

    // <link> has multiple forms: prefer rel="alternate" (or no rel) with an
    // href attribute. Track the best candidate and any torrent enclosure.
    std::string alternate_link;
    std::optional<std::string> torrent_url;
    for_each_tag(body, "link", [&](const std::string& attrs) {
        // Extract rel, type, href.
        auto find_attr = [&](std::string_view name) -> std::optional<std::string> {
            const std::string pat = std::string{name} + R"_(\s*=\s*"([^"]*)")_";
            std::regex r{pat, std::regex::icase};
            std::smatch m;
            if (std::regex_search(attrs, m, r)) return m[1].str();
            return std::nullopt;
        };
        const auto href = find_attr("href");
        if (!href) return;
        const auto rel  = find_attr("rel").value_or("alternate");
        const auto type = find_attr("type").value_or("");
        if (type == "application/x-bittorrent") {
            torrent_url = *href;
        } else if (rel == "alternate" && alternate_link.empty()) {
            alternate_link = *href;
        }
    });
    entry.link = std::move(alternate_link);
    entry.torrent_url = std::move(torrent_url);
    if (entry.guid.empty()) entry.guid = entry.link;

    if (entry.description) {
        if (auto magnet = extract_magnet(*entry.description)) {
            entry.magnet = std::move(*magnet);
        }
    }
    if (!entry.magnet) {
        if (auto magnet = extract_magnet(body)) {
            entry.magnet = std::move(*magnet);
        }
    }

    if (auto pub = first_tag_text(body, "published")) {
        entry.published_at = parse_rfc3339(*pub);
    } else if (auto upd = first_tag_text(body, "updated")) {
        entry.published_at = parse_rfc3339(*upd);
    }
    return entry;
}

Result<RssDocument> parse_atom(std::string_view xml) {
    RssDocument doc;

    const auto first_entry = std::cregex_iterator(xml.data(), xml.data() + xml.size(),
                                                  atom_entry_regex());
    std::string_view header_view = xml;
    if (first_entry != std::cregex_iterator()) {
        const auto offset = static_cast<std::size_t>((*first_entry).position(0));
        header_view = xml.substr(0, offset);
    }
    if (auto v = first_tag_text(header_view, "title")) doc.title = std::move(*v);
    if (auto v = first_tag_text(header_view, "subtitle")) doc.description = std::move(*v);

    for_each_tag(header_view, "link", [&](const std::string& attrs) {
        auto find_attr = [&](std::string_view name) -> std::optional<std::string> {
            const std::string pat = std::string{name} + R"_(\s*=\s*"([^"]*)")_";
            std::regex r{pat, std::regex::icase};
            std::smatch m;
            if (std::regex_search(attrs, m, r)) return m[1].str();
            return std::nullopt;
        };
        if (!doc.link.empty()) return;
        const auto href = find_attr("href");
        if (!href) return;
        const auto rel = find_attr("rel").value_or("alternate");
        if (rel == "alternate") doc.link = *href;
    });

    auto begin = std::cregex_iterator(xml.data(), xml.data() + xml.size(),
                                      atom_entry_regex());
    auto end   = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
        doc.entries.emplace_back(parse_atom_entry((*it)[1].str()));
    }
    return doc;
}

}  // namespace

Result<RssDocument> RssDocumentParser::parse(std::string_view xml) {
    if (xml.empty()) {
        return error(ErrorCode::Deserialization, "empty RSS document");
    }
    const bool is_rss  = std::regex_search(xml.data(), xml.data() + xml.size(),
                                           rss_root_regex());
    const bool is_atom = std::regex_search(xml.data(), xml.data() + xml.size(),
                                           atom_root_regex());
    if (is_rss) return parse_rss(xml);
    if (is_atom) return parse_atom(xml);
    return error(ErrorCode::Deserialization,
                 "document is neither RSS 2.0 nor Atom 1.0");
}

}  // namespace cmlb::infrastructure::rss
