#include <cmlb/core/formatting.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace cmlb::core {

namespace {

constexpr std::int64_t KiB = 1024;
constexpr std::int64_t MiB = 1024 * KiB;
constexpr std::int64_t GiB = 1024 * MiB;
constexpr std::int64_t TiB = 1024 * GiB;
constexpr std::int64_t PiB = 1024 * TiB;

constexpr std::int64_t KB = 1000;
constexpr std::int64_t MB = 1000 * KB;
constexpr std::int64_t GB = 1000 * MB;
constexpr std::int64_t TB = 1000 * GB;
constexpr std::int64_t PB = 1000 * TB;

double clamp_fraction(double f) noexcept {
    if (!(f == f)) return 0.0;  // NaN guard
    return std::clamp(f, 0.0, 1.0);
}

std::string format_binary(std::int64_t bytes, std::string_view suffix) {
    const bool negative = bytes < 0;
    const auto abs_bytes = negative ? -static_cast<long double>(bytes)
                                    : static_cast<long double>(bytes);

    struct Unit {
        long double scale;
        const char* label;
    };
    static constexpr std::array<Unit, 6> kUnits{{
        {1.0L,                              "B"},
        {static_cast<long double>(KiB),     "KiB"},
        {static_cast<long double>(MiB),     "MiB"},
        {static_cast<long double>(GiB),     "GiB"},
        {static_cast<long double>(TiB),     "TiB"},
        {static_cast<long double>(PiB),     "PiB"},
    }};

    std::size_t idx = 0;
    for (std::size_t i = 1; i < kUnits.size(); ++i) {
        if (abs_bytes >= kUnits[i].scale) idx = i;
        else break;
    }

    const long double value = abs_bytes / kUnits[idx].scale;
    const char* sign = negative ? "-" : "";

    if (idx == 0) {
        return fmt::format("{}{} {}{}", sign,
                           static_cast<std::int64_t>(value),
                           kUnits[idx].label, suffix);
    }
    return fmt::format("{}{:.2f} {}{}", sign,
                       static_cast<double>(value),
                       kUnits[idx].label, suffix);
}

std::string format_decimal(std::int64_t bytes, std::string_view suffix) {
    const bool negative = bytes < 0;
    const auto abs_bytes = negative ? -static_cast<long double>(bytes)
                                    : static_cast<long double>(bytes);

    struct Unit {
        long double scale;
        const char* label;
    };
    static constexpr std::array<Unit, 6> kUnits{{
        {1.0L,                            "B"},
        {static_cast<long double>(KB),    "KB"},
        {static_cast<long double>(MB),    "MB"},
        {static_cast<long double>(GB),    "GB"},
        {static_cast<long double>(TB),    "TB"},
        {static_cast<long double>(PB),    "PB"},
    }};

    std::size_t idx = 0;
    for (std::size_t i = 1; i < kUnits.size(); ++i) {
        if (abs_bytes >= kUnits[i].scale) idx = i;
        else break;
    }

    const long double value = abs_bytes / kUnits[idx].scale;
    const char* sign = negative ? "-" : "";

    if (idx == 0) {
        return fmt::format("{}{} {}{}", sign,
                           static_cast<std::int64_t>(value),
                           kUnits[idx].label, suffix);
    }
    return fmt::format("{}{:.2f} {}{}", sign,
                       static_cast<double>(value),
                       kUnits[idx].label, suffix);
}

}  // namespace

std::string format_bytes(std::int64_t bytes) {
    return format_binary(bytes, "");
}

std::string format_decimal_bytes(std::int64_t bytes) {
    return format_decimal(bytes, "");
}

std::string format_duration(std::chrono::seconds duration) {
    auto total = duration.count();
    if (total < 0) total = -total;

    const std::int64_t days = total / 86'400;
    total %= 86'400;
    const std::int64_t hours = total / 3'600;
    total %= 3'600;
    const std::int64_t minutes = total / 60;
    const std::int64_t seconds = total % 60;

    if (days > 0) {
        return fmt::format("{}d {}h {}m {}s", days, hours, minutes, seconds);
    }
    if (hours > 0) {
        return fmt::format("{}h {}m {}s", hours, minutes, seconds);
    }
    if (minutes > 0) {
        return fmt::format("{}m {}s", minutes, seconds);
    }
    return fmt::format("{}s", seconds);
}

std::string format_eta(std::chrono::seconds duration) {
    if (duration <= std::chrono::seconds{0}) {
        return "--";
    }
    auto total = duration.count();
    const std::int64_t days = total / 86'400;
    total %= 86'400;
    const std::int64_t hours = total / 3'600;
    total %= 3'600;
    const std::int64_t minutes = total / 60;

    if (days > 0) {
        return fmt::format("~{}d {}h", days, hours);
    }
    if (hours > 0) {
        return fmt::format("~{}h {}m", hours, minutes);
    }
    if (minutes > 0) {
        return fmt::format("~{}m {}s", minutes, total % 60);
    }
    return fmt::format("~{}s", total);
}

std::string render_progress_bar(double fraction, std::size_t width,
                                char filled, char empty) {
    const double f = clamp_fraction(fraction);
    if (width == 0) return "[]";

    const auto fill_count = static_cast<std::size_t>(
        std::floor(f * static_cast<double>(width) + 0.0));
    const std::size_t clamped = std::min(fill_count, width);

    std::string out;
    out.reserve(width + 2);
    out.push_back('[');
    out.append(clamped, filled);
    out.append(width - clamped, empty);
    out.push_back(']');
    return out;
}

std::string format_rate(std::int64_t bytes_per_second) {
    if (bytes_per_second < 0) bytes_per_second = 0;
    return format_binary(bytes_per_second, "/s");
}

std::string format_percent(double fraction, int decimals) {
    const double f = clamp_fraction(fraction) * 100.0;
    if (decimals < 0) decimals = 0;
    return fmt::format("{:.{}f}%", f, decimals);
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out.append("&amp;");  break;
            case '<': out.append("&lt;");   break;
            case '>': out.append("&gt;");   break;
            default:  out.push_back(ch);    break;
        }
    }
    return out;
}

std::string truncate_for_display(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string{text};
    }
    constexpr std::string_view kEllipsis{"..."};
    if (max_bytes <= kEllipsis.size()) {
        return std::string{kEllipsis.substr(0, max_bytes)};
    }
    std::size_t cut = max_bytes - kEllipsis.size();
    // UTF-8 safety: back up while sitting on a continuation byte (10xxxxxx)
    // so a multibyte sequence is never split. Terminates because the start
    // of any UTF-8 sequence is either a leading byte or the string start.
    while (cut > 0
           && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string out;
    out.reserve(cut + kEllipsis.size());
    out.append(text.substr(0, cut));
    out.append(kEllipsis);
    return out;
}

std::string_view friendly_error_label(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:                return "OK";
        case ErrorCode::InvalidArgument:     return "Invalid argument";
        case ErrorCode::InvalidConfiguration:return "Invalid configuration";
        case ErrorCode::InvalidState:        return "Invalid state";
        case ErrorCode::NotFound:            return "Not found";
        case ErrorCode::AlreadyExists:       return "Already exists";
        case ErrorCode::PermissionDenied:    return "Permission denied";
        case ErrorCode::Unauthenticated:     return "Authentication required";
        case ErrorCode::Cancelled:           return "Cancelled";
        case ErrorCode::DeadlineExceeded:    return "Deadline exceeded";
        case ErrorCode::ResourceExhausted:   return "Resource exhausted";
        case ErrorCode::QuotaExceeded:       return "Quota exceeded";
        case ErrorCode::Network:             return "Network error";
        case ErrorCode::Timeout:             return "Timed out";
        case ErrorCode::Io:                  return "I/O error";
        case ErrorCode::FileSystem:          return "Filesystem error";
        case ErrorCode::Serialization:       return "Serialization failed";
        case ErrorCode::Deserialization:     return "Deserialization failed";
        case ErrorCode::JsonParse:           return "Invalid JSON";
        case ErrorCode::TelegramApi:         return "Telegram error";
        case ErrorCode::Aria2Rpc:            return "aria2 error";
        case ErrorCode::QbittorrentApi:      return "qBittorrent error";
        case ErrorCode::GoogleDriveApi:      return "Google Drive error";
        case ErrorCode::RcloneInvocation:    return "rclone error";
        case ErrorCode::Database:            return "Database error";
        case ErrorCode::Migration:           return "Schema migration failed";
        case ErrorCode::SubprocessFailed:    return "Helper process failed";
        case ErrorCode::MediaProcessing:     return "Media processing failed";
        case ErrorCode::ArchiveProcessing:   return "Archive processing failed";
        case ErrorCode::Internal:            return "Internal error";
        case ErrorCode::Unknown:             return "Error";
    }
    return "Error";
}

}  // namespace cmlb::core
