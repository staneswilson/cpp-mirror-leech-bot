#include <cmlb/core/error.hpp>

#include <filesystem>
#include <ostream>
#include <string_view>

namespace cmlb::core {

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None:               return "None";
        case ErrorCode::InvalidArgument:    return "InvalidArgument";
        case ErrorCode::InvalidConfiguration: return "InvalidConfiguration";
        case ErrorCode::InvalidState:       return "InvalidState";
        case ErrorCode::NotFound:           return "NotFound";
        case ErrorCode::AlreadyExists:      return "AlreadyExists";
        case ErrorCode::PermissionDenied:   return "PermissionDenied";
        case ErrorCode::Unauthenticated:    return "Unauthenticated";
        case ErrorCode::Cancelled:          return "Cancelled";
        case ErrorCode::DeadlineExceeded:   return "DeadlineExceeded";
        case ErrorCode::ResourceExhausted:  return "ResourceExhausted";
        case ErrorCode::QuotaExceeded:      return "QuotaExceeded";
        case ErrorCode::Network:            return "Network";
        case ErrorCode::Timeout:            return "Timeout";
        case ErrorCode::Io:                 return "Io";
        case ErrorCode::FileSystem:         return "FileSystem";
        case ErrorCode::Serialization:      return "Serialization";
        case ErrorCode::Deserialization:    return "Deserialization";
        case ErrorCode::JsonParse:          return "JsonParse";
        case ErrorCode::TelegramApi:        return "TelegramApi";
        case ErrorCode::Aria2Rpc:           return "Aria2Rpc";
        case ErrorCode::QbittorrentApi:     return "QbittorrentApi";
        case ErrorCode::GoogleDriveApi:     return "GoogleDriveApi";
        case ErrorCode::RcloneInvocation:   return "RcloneInvocation";
        case ErrorCode::Database:           return "Database";
        case ErrorCode::Migration:          return "Migration";
        case ErrorCode::SubprocessFailed:   return "SubprocessFailed";
        case ErrorCode::MediaProcessing:    return "MediaProcessing";
        case ErrorCode::ArchiveProcessing:  return "ArchiveProcessing";
        case ErrorCode::Internal:           return "Internal";
        case ErrorCode::Unknown:            return "Unknown";
    }
    // Unreachable: every enumerator is handled above. The compiler will warn
    // (`-Wswitch`) if a new enumerator is added without an arm — that warning
    // is the point. We still return something to satisfy `-Wreturn-type`.
    return "Unknown";
}

std::ostream& operator<<(std::ostream& os, const AppError& err) {
    const auto file = std::filesystem::path{err.location.file_name()}.filename().string();
    os << "code(" << error_code_name(err.code) << "): " << err.message
       << " at " << file << ':' << err.location.line();
    return os;
}

}  // namespace cmlb::core
