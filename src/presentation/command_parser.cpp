// ---------------------------------------------------------------------------
// command_parser.cpp
//
// Pure command parser. No I/O, no asio, no use cases.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <cmlb/presentation/command_parser.hpp>

namespace cmlb::presentation {

namespace {

/// Returns @p text with leading and trailing ASCII whitespace removed.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    auto is_ws = [](unsigned char ch) noexcept {
        return std::isspace(ch) != 0;
    };
    std::size_t first = 0;
    while (first < text.size() && is_ws(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && is_ws(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

/// Lower-cases an ASCII string in place. Non-ASCII bytes pass through.
[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

} // namespace

std::optional<CommandRequest> CommandParser::parse(std::string_view text,
                                                   cmlb::domain::UserId sender,
                                                   cmlb::domain::ChatId chat,
                                                   cmlb::domain::MessageId msg) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty() || trimmed.front() != '/') {
        return std::nullopt;
    }

    // Drop the leading slash and split on the first run of whitespace.
    const std::string_view body = trimmed.substr(1);
    if (body.empty()) {
        return std::nullopt; // bare "/" is not a command
    }

    const auto ws_pos = std::ranges::find_if(body, [](char ch) noexcept {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    });

    std::string_view head =
        body.substr(0, static_cast<std::size_t>(std::distance(body.begin(), ws_pos)));
    std::string_view tail =
        (ws_pos == body.end())
            ? std::string_view{}
            : body.substr(static_cast<std::size_t>(std::distance(body.begin(), ws_pos) + 1));

    // Strip the `@BotUsername` suffix if present.
    if (const auto at_pos = head.find('@'); at_pos != std::string_view::npos) {
        head = head.substr(0, at_pos);
    }

    if (head.empty()) {
        return std::nullopt;
    }

    CommandRequest request{
        .command = to_lower_ascii(head),
        .arguments = std::string{trim(tail)},
        .full_text = std::string{text},
        .sender = sender,
        .chat = chat,
        .source_message = msg,
    };
    return request;
}

} // namespace cmlb::presentation
