#pragma once

#include <expected>
#include <ostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace cmlb::core {

/// Stable, scoped error codes used across the entire CMLB codebase.
///
/// Codes are organized in coarse "families" so callers can branch on a single
/// value rather than parsing strings. All recoverable failure paths return
/// `Result<T>` populated with one of these codes.
enum class ErrorCode {
    /// Sentinel — never produced by a failing path; reserved for "ok" markers.
    None,

    /// A caller-provided argument is malformed or out of range.
    InvalidArgument,
    /// The on-disk / in-memory configuration object failed validation.
    InvalidConfiguration,
    /// The receiving object cannot service the call in its current state.
    InvalidState,

    /// A requested resource (file, key, row, task) does not exist.
    NotFound,
    /// A resource that must be unique already exists.
    AlreadyExists,
    /// Caller is authenticated but not authorized for this operation.
    PermissionDenied,
    /// Caller could not be authenticated (no/expired credentials).
    Unauthenticated,

    /// Operation was cancelled before it could complete.
    Cancelled,
    /// Operation exceeded its deadline.
    DeadlineExceeded,
    /// Operation could not proceed because a resource pool is saturated.
    ResourceExhausted,
    /// Operation refused because a quota (e.g. drive storage) is exhausted.
    QuotaExceeded,

    /// Transport-level network error (connect, send, recv).
    Network,
    /// Operation timed out waiting for a peer/external service.
    Timeout,
    /// Generic I/O error (read/write/seek).
    Io,
    /// Filesystem-specific error (permissions, ENOSPC, ENOENT on path ops).
    FileSystem,

    /// Failed to serialize a value into a wire/persistence format.
    Serialization,
    /// Failed to deserialize a value from a wire/persistence format.
    Deserialization,
    /// nlohmann::json parsing failed.
    JsonParse,

    /// TDLib / Telegram Bot API returned a failure.
    TelegramApi,
    /// aria2 JSON-RPC returned a failure or the channel is broken.
    Aria2Rpc,
    /// qBittorrent Web API returned a failure.
    QbittorrentApi,
    /// Google Drive API returned a failure.
    GoogleDriveApi,
    /// `rclone` subprocess invocation failed.
    RcloneInvocation,

    /// Generic database error from sqlite-modern-cpp or the engine.
    Database,
    /// Database schema migration failed.
    Migration,

    /// A non-rclone subprocess returned a non-zero exit code.
    SubprocessFailed,
    /// FFmpeg/thumbnail/metadata extraction step failed.
    MediaProcessing,
    /// Archive extraction or creation failed.
    ArchiveProcessing,

    /// An invariant was violated (bug). Treat as fatal in callers.
    Internal,
    /// Catch-all for unclassified failures.
    Unknown
};

/// Returns the C++ enumerator name for a given `ErrorCode`.
///
/// Every enumerator is exhaustively mapped — no value falls through to a
/// "Unknown" string unless the code itself is `Unknown`.
[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

/// Rich application-level error carrying a code, message, and the source
/// location where the error was constructed.
struct AppError {
    /// Machine-readable failure classification.
    ErrorCode code{ErrorCode::Unknown};
    /// Human-readable diagnostic. May span multiple lines.
    std::string message;
    /// Source location of the construction site (typically the failing call).
    std::source_location location;

    /// Constructs an `AppError`, capturing the call site by default.
    AppError(ErrorCode code_,
             std::string message_,
             std::source_location loc = std::source_location::current()) noexcept
        : code{code_}, message{std::move(message_)}, location{loc} {}
};

/// Stream formatter: `"code(Name): message at file:line"`.
std::ostream& operator<<(std::ostream& os, const AppError& err);

/// Canonical result type for any fallible operation in CMLB.
template <typename T>
using Result = std::expected<T, AppError>;

/// Builds an `std::unexpected<AppError>` capturing the call site.
///
/// Use as: `return cmlb::core::error(ErrorCode::Network, "connect failed");`.
[[nodiscard]] inline auto error(
    ErrorCode code,
    std::string msg,
    std::source_location loc = std::source_location::current()) {
    return std::unexpected(AppError{code, std::move(msg), loc});
}

}  // namespace cmlb::core
