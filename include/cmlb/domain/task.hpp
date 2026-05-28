#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>

/// @file task.hpp
/// @brief Task aggregate root and its explicit state machine.

namespace cmlb::domain {

/// Lifecycle state of a single download/upload task.
enum class TaskState {
    Queued,      ///< Newly created, awaiting downloader pickup.
    Downloading, ///< Downloader is fetching bytes.
    Processing,  ///< Archive extraction / media processing intermediate phase.
    Uploading,   ///< Uploader is pushing bytes to the configured destination.
    Completed,   ///< Terminal: upload finished successfully.
    Failed,      ///< Terminal: aborted with an error message.
    Cancelled,   ///< Terminal: cancelled by user or system.
};

[[nodiscard]] std::string_view to_string(TaskState state) noexcept;

/// What the task is supposed to do with the downloaded payload.
enum class TaskKind {
    Mirror, ///< Download then upload to cloud storage (rclone / GDrive).
    Leech,  ///< Download then upload to Telegram.
    Clone,  ///< Server-side GDrive copy (no local download/upload).
};

/// Which downloader backend a Task is bound to. `None` indicates that the
/// downloader hasn't yet accepted the request (the task is still in `Queued`
/// or the downloader call has not yet returned).
enum class DownloaderKind {
    None,
    Aria2,
    Qbittorrent,
};

[[nodiscard]] std::string_view to_string(DownloaderKind kind) noexcept;

/// Immutable identifying / contextual metadata for a task.
struct TaskMetadata {
    TaskId id;
    UserId user;
    ChatId chat;
    MessageId status_message; ///< Bot's live progress message in @p chat.
    TaskKind kind;
    std::string source_url; ///< Original URL or magnet the user sent.
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

/// Aggregate root. Encapsulates the explicit state machine. All transitions
/// return Result<void>; invalid transitions yield ErrorCode::InvalidState.
class Task {
public:
    explicit Task(TaskMetadata metadata);

    [[nodiscard]] const TaskMetadata& metadata() const noexcept {
        return metadata_;
    }

    [[nodiscard]] TaskState state() const noexcept {
        return state_;
    }

    [[nodiscard]] std::optional<std::string_view> error_message() const noexcept;

    [[nodiscard]] std::chrono::system_clock::time_point updated_at() const noexcept {
        return metadata_.updated_at;
    }

    /// @name State transitions
    /// Each transition validates the current state, performs the change, and
    /// updates the @c updated_at timestamp. Invalid transitions are reported
    /// as @c ErrorCode::InvalidState without mutating internal state.
    /// @{
    [[nodiscard]] cmlb::core::Result<void> start_download();
    [[nodiscard]] cmlb::core::Result<void> begin_processing();
    [[nodiscard]] cmlb::core::Result<void> begin_upload();
    [[nodiscard]] cmlb::core::Result<void> mark_completed();
    [[nodiscard]] cmlb::core::Result<void> mark_failed(std::string reason);
    [[nodiscard]] cmlb::core::Result<void> mark_cancelled();
    /// @}

    /// True iff the current state is Completed, Failed, or Cancelled.
    [[nodiscard]] bool is_terminal() const noexcept;

    /// @name Downloader binding
    /// Records (or queries) which downloader accepted the task and the Gid it
    /// returned. This is mutable metadata — it is set once the downloader
    /// responds and read back when the task is cancelled, paused, or resumed.
    /// @{

    /// Returns the downloader-assigned id once `attach_downloader` has been
    /// called, otherwise `std::nullopt`.
    [[nodiscard]] std::optional<Gid> downloader_id() const noexcept;

    /// Returns the downloader backend the task was dispatched to. Defaults to
    /// `DownloaderKind::None` until `attach_downloader` runs.
    [[nodiscard]] DownloaderKind downloader_kind() const noexcept;

    /// Records the downloader assignment. The pair @p (id, kind) is taken as
    /// a unit so a partial set (e.g. just a Gid with `None` kind) is never
    /// representable. Bumps `updated_at`.
    void attach_downloader(Gid id, DownloaderKind kind) noexcept;

    /// @}

private:
    TaskMetadata metadata_;
    TaskState state_{TaskState::Queued};
    std::optional<std::string> error_message_;
    std::optional<Gid> downloader_id_;
    DownloaderKind downloader_kind_{DownloaderKind::None};

    void touch_() noexcept;
};

} // namespace cmlb::domain
