#include <chrono>
#include <string>
#include <utility>

#include <cmlb/domain/task.hpp>

namespace cmlb::domain {

namespace {

[[nodiscard]] auto invalid_transition(std::string_view from,
                                      std::string_view to,
                                      std::source_location loc = std::source_location::current()) {
    return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                             "Invalid task transition: " + std::string{from} + " -> "
                                 + std::string{to},
                             loc);
}

} // namespace

std::string_view to_string(TaskState state) noexcept {
    switch (state) {
    case TaskState::Queued:
        return "Queued";
    case TaskState::Downloading:
        return "Downloading";
    case TaskState::Processing:
        return "Processing";
    case TaskState::Uploading:
        return "Uploading";
    case TaskState::Completed:
        return "Completed";
    case TaskState::Failed:
        return "Failed";
    case TaskState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

std::string_view to_string(DownloaderKind kind) noexcept {
    switch (kind) {
    case DownloaderKind::None:
        return "None";
    case DownloaderKind::Aria2:
        return "Aria2";
    case DownloaderKind::Qbittorrent:
        return "Qbittorrent";
    }
    return "Unknown";
}

Task::Task(TaskMetadata metadata) : metadata_{std::move(metadata)} {
}

std::optional<std::string_view> Task::error_message() const noexcept {
    if (error_message_.has_value()) {
        return std::string_view{*error_message_};
    }
    return std::nullopt;
}

bool Task::is_terminal() const noexcept {
    return state_ == TaskState::Completed || state_ == TaskState::Failed
           || state_ == TaskState::Cancelled;
}

void Task::touch_() noexcept {
    metadata_.updated_at = std::chrono::system_clock::now();
}

cmlb::core::Result<void> Task::start_download() {
    if (state_ != TaskState::Queued) {
        return invalid_transition(to_string(state_), to_string(TaskState::Downloading));
    }
    state_ = TaskState::Downloading;
    touch_();
    return {};
}

cmlb::core::Result<void> Task::begin_processing() {
    if (state_ != TaskState::Downloading) {
        return invalid_transition(to_string(state_), to_string(TaskState::Processing));
    }
    state_ = TaskState::Processing;
    touch_();
    return {};
}

cmlb::core::Result<void> Task::begin_upload() {
    if (state_ != TaskState::Downloading && state_ != TaskState::Processing) {
        return invalid_transition(to_string(state_), to_string(TaskState::Uploading));
    }
    state_ = TaskState::Uploading;
    touch_();
    return {};
}

cmlb::core::Result<void> Task::mark_completed() {
    if (state_ != TaskState::Uploading) {
        return invalid_transition(to_string(state_), to_string(TaskState::Completed));
    }
    state_ = TaskState::Completed;
    touch_();
    return {};
}

cmlb::core::Result<void> Task::mark_failed(std::string reason) {
    if (is_terminal()) {
        return invalid_transition(to_string(state_), to_string(TaskState::Failed));
    }
    error_message_ = std::move(reason);
    state_ = TaskState::Failed;
    touch_();
    return {};
}

cmlb::core::Result<void> Task::mark_cancelled() {
    if (is_terminal()) {
        return invalid_transition(to_string(state_), to_string(TaskState::Cancelled));
    }
    state_ = TaskState::Cancelled;
    touch_();
    return {};
}

std::optional<Gid> Task::downloader_id() const noexcept {
    return downloader_id_;
}

DownloaderKind Task::downloader_kind() const noexcept {
    return downloader_kind_;
}

void Task::attach_downloader(Gid id, DownloaderKind kind) noexcept {
    downloader_id_ = std::move(id);
    downloader_kind_ = kind;
    touch_();
}

} // namespace cmlb::domain
