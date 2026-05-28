#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include <cmlb/core/error.hpp>

/// @file upload_destination.hpp
/// @brief Enumeration of supported upload destinations + string conversion.

namespace cmlb::domain {

enum class UploadDestination {
    Telegram,
    GoogleDrive,
    Rclone,
};

[[nodiscard]] inline std::string_view to_string(UploadDestination dest) noexcept {
    switch (dest) {
        case UploadDestination::Telegram:    return "telegram";
        case UploadDestination::GoogleDrive: return "gdrive";
        case UploadDestination::Rclone:      return "rclone";
    }
    return "unknown";
}

/// Case-insensitive parser. Recognised aliases:
///   - telegram, tg, leech                       → Telegram
///   - gdrive, googledrive, google_drive, drive  → GoogleDrive
///   - rclone, mirror                            → Rclone
[[nodiscard]] inline cmlb::core::Result<UploadDestination> parse_upload_destination(
    std::string_view input,
    std::source_location loc = std::source_location::current()) {
    std::string lower;
    lower.reserve(input.size());
    for (const char ch : input) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }

    static constexpr std::array telegram_aliases{
        std::string_view{"telegram"}, std::string_view{"tg"}, std::string_view{"leech"}};
    static constexpr std::array gdrive_aliases{
        std::string_view{"gdrive"}, std::string_view{"googledrive"},
        std::string_view{"google_drive"}, std::string_view{"drive"}};
    static constexpr std::array rclone_aliases{
        std::string_view{"rclone"}, std::string_view{"mirror"}};

    if (std::ranges::find(telegram_aliases, lower) != telegram_aliases.end()) {
        return UploadDestination::Telegram;
    }
    if (std::ranges::find(gdrive_aliases, lower) != gdrive_aliases.end()) {
        return UploadDestination::GoogleDrive;
    }
    if (std::ranges::find(rclone_aliases, lower) != rclone_aliases.end()) {
        return UploadDestination::Rclone;
    }

    return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                             "Unknown upload destination: " + std::string{input}, loc);
}

}  // namespace cmlb::domain
