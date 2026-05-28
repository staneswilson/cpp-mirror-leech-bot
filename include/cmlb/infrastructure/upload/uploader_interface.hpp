#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/byte_size.hpp>
#include <cmlb/domain/identifiers.hpp>

/// @file uploader_interface.hpp
/// @brief Polymorphic upload-adapter contract.
///
/// Three concrete implementations live alongside this header:
///   * `TelegramUploader`     — sends files back as Telegram media/documents.
///   * `GoogleDriveUploader`  — uploads to Drive via service-account JWT.
///   * `RcloneUploader`       — shells out to the `rclone` binary.
///
/// Every method returning `Result<T>` is `[[nodiscard]]`; long-running
/// `awaitable<>`s honour the caller's `cancellation_slot` (see
/// `cmlb/core/cancellation.hpp`).

namespace cmlb::infrastructure::upload {

/// Per-call upload parameters. Empty optionals mean "not provided"; concrete
/// uploaders pick the fields that apply to their backend.
struct UploadConfig {
    /// Telegram destination chat (mandatory for `TelegramUploader`).
    std::optional<cmlb::domain::ChatId> chat_id;
    /// Drive parent folder id; overrides the configured default when set.
    std::optional<std::string> folder_id;
    /// rclone destination spec, e.g. `"gdrive:Backups/movies"`.
    std::optional<std::string> rclone_path;
    /// Force send-as-document even for known media types.
    bool as_document{false};
    /// Optional path to a JPEG thumbnail (Telegram only).
    std::optional<std::filesystem::path> thumbnail_path;
    /// Optional message caption / metadata to attach to the upload.
    std::optional<std::string> caption;
    /// Split threshold for backends that need it (Telegram 2 GB by default).
    cmlb::domain::ByteSize split_size{
        cmlb::domain::ByteSize::from_unchecked(2'000'000'000)};
};

/// Snapshot delivered to the `UploadProgressHandler`. Throttled to at most one
/// callback per second by the adapter.
struct UploadProgress {
    /// File currently being uploaded (basename, not absolute path).
    std::string file_name;
    /// Total bytes scheduled for upload (`-1` if unknown).
    std::int64_t total_bytes{0};
    /// Bytes successfully delivered so far.
    std::int64_t uploaded_bytes{0};
    /// Instantaneous throughput in bytes/second (`0` until first sample).
    std::int64_t speed_bps{0};
    /// Estimated time remaining (`0` when not yet computable).
    std::chrono::seconds eta{0};
    /// 1-based index of the chunk/part currently being sent.
    int current_part{0};
    /// Total number of chunks/parts for the active file.
    int total_parts{1};

    /// `uploaded_bytes / total_bytes` clamped to `[0, 1]`. Returns `0.0` when
    /// `total_bytes <= 0`.
    [[nodiscard]] double progress_percent() const noexcept;
};

/// Result of a successful upload. `link` is empty when the backend has no
/// shareable URL concept (e.g. raw Telegram documents).
struct UploadResult {
    /// Telegram file_id, GDrive file id, or rclone destination path.
    std::string file_id;
    /// Shareable URL when applicable; empty string otherwise.
    std::string link;
    /// Final upload size in bytes.
    std::int64_t size{0};
    /// Wall-clock duration of the upload from `co_await` to completion.
    std::chrono::milliseconds duration{0};
};

/// Progress callback signature. Invoked on the executor's strand; handlers
/// must not block.
using UploadProgressHandler = std::function<void(const UploadProgress&)>;

/// Abstract uploader contract. Implementations live in this namespace.
class UploaderInterface {
public:
    virtual ~UploaderInterface() = default;

    UploaderInterface() = default;
    UploaderInterface(const UploaderInterface&)            = delete;
    UploaderInterface& operator=(const UploaderInterface&) = delete;
    UploaderInterface(UploaderInterface&&)                 = delete;
    UploaderInterface& operator=(UploaderInterface&&)      = delete;

    /// Uploads a single file. Honors `co_await this_coro::cancellation_state`.
    /// On cancellation, partial state (e.g. GDrive resumable sessions) is
    /// cleaned up before returning `error(ErrorCode::Cancelled, ...)`.
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<UploadResult>>
    upload_file(std::filesystem::path path,
                UploadConfig config,
                UploadProgressHandler on_progress) = 0;

    /// Recursively uploads `path` (must be a directory). Implementations
    /// preserve relative folder structure where the backend supports it.
    [[nodiscard]] virtual boost::asio::awaitable<
        cmlb::core::Result<std::vector<UploadResult>>>
    upload_directory(std::filesystem::path path,
                     UploadConfig config,
                     UploadProgressHandler on_progress) = 0;

    /// Human-readable adapter name (`"telegram"`, `"gdrive"`, `"rclone"`).
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Returns true when the adapter has the credentials / binary it needs to
    /// service uploads. Cached by implementations — cheap to call repeatedly.
    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
};

}  // namespace cmlb::infrastructure::upload
