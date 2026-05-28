#include <chrono>
#include <functional>
#include <string>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/task.hpp>

using cmlb::core::ErrorCode;
using cmlb::domain::ChatId;
using cmlb::domain::MessageId;
using cmlb::domain::Task;
using cmlb::domain::TaskId;
using cmlb::domain::TaskKind;
using cmlb::domain::TaskMetadata;
using cmlb::domain::TaskState;
using cmlb::domain::UserId;

namespace {

[[nodiscard]] TaskMetadata sample_metadata() {
    const auto now = std::chrono::system_clock::now();
    return TaskMetadata{
        .id = TaskId{std::string{"01890b3c-7f0c-7e0e-bb91-aa8b1a0d3f01"}},
        .user = UserId{42},
        .chat = ChatId{-1001234567890},
        .status_message = MessageId{777},
        .kind = TaskKind::Mirror,
        .source_url = "https://example.com/file.iso",
        .created_at = now,
        .updated_at = now,
    };
}

/// Drives the task to a specific state via the canonical happy path. Used to
/// stage transition-table cases.
void drive_to(Task& task, TaskState target) {
    switch (target) {
    case TaskState::Queued:
        return;
    case TaskState::Downloading:
        REQUIRE(task.start_download().has_value());
        return;
    case TaskState::Processing:
        REQUIRE(task.start_download().has_value());
        REQUIRE(task.begin_processing().has_value());
        return;
    case TaskState::Uploading:
        REQUIRE(task.start_download().has_value());
        REQUIRE(task.begin_processing().has_value());
        REQUIRE(task.begin_upload().has_value());
        return;
    case TaskState::Completed:
        REQUIRE(task.start_download().has_value());
        REQUIRE(task.begin_processing().has_value());
        REQUIRE(task.begin_upload().has_value());
        REQUIRE(task.mark_completed().has_value());
        return;
    case TaskState::Failed:
        REQUIRE(task.mark_failed("oops").has_value());
        return;
    case TaskState::Cancelled:
        REQUIRE(task.mark_cancelled().has_value());
        return;
    }
}

using Action = std::function<cmlb::core::Result<void>(Task&)>;

const Action k_start_download = [](Task& t) {
    return t.start_download();
};
const Action k_begin_processing = [](Task& t) {
    return t.begin_processing();
};
const Action k_begin_upload = [](Task& t) {
    return t.begin_upload();
};
const Action k_mark_completed = [](Task& t) {
    return t.mark_completed();
};
const Action k_mark_failed = [](Task& t) {
    return t.mark_failed("err");
};
const Action k_mark_cancelled = [](Task& t) {
    return t.mark_cancelled();
};

} // namespace

TEST_CASE("Task is constructed in Queued state", "[domain][task]") {
    Task task{sample_metadata()};
    REQUIRE(task.state() == TaskState::Queued);
    REQUIRE_FALSE(task.is_terminal());
    REQUIRE_FALSE(task.error_message().has_value());
}

TEST_CASE("to_string(TaskState) returns stable identifiers", "[domain][task]") {
    REQUIRE(to_string(TaskState::Queued) == "Queued");
    REQUIRE(to_string(TaskState::Downloading) == "Downloading");
    REQUIRE(to_string(TaskState::Processing) == "Processing");
    REQUIRE(to_string(TaskState::Uploading) == "Uploading");
    REQUIRE(to_string(TaskState::Completed) == "Completed");
    REQUIRE(to_string(TaskState::Failed) == "Failed");
    REQUIRE(to_string(TaskState::Cancelled) == "Cancelled");
}

TEST_CASE("Happy path: Queued -> Downloading -> Processing -> Uploading -> Completed",
          "[domain][task]") {
    Task task{sample_metadata()};
    const auto t0 = task.updated_at();

    REQUIRE(task.start_download().has_value());
    REQUIRE(task.state() == TaskState::Downloading);

    REQUIRE(task.begin_processing().has_value());
    REQUIRE(task.state() == TaskState::Processing);

    REQUIRE(task.begin_upload().has_value());
    REQUIRE(task.state() == TaskState::Uploading);

    REQUIRE(task.mark_completed().has_value());
    REQUIRE(task.state() == TaskState::Completed);
    REQUIRE(task.is_terminal());
    REQUIRE(task.updated_at() >= t0);
}

TEST_CASE("Shortcut path: Downloading -> Uploading skips Processing", "[domain][task]") {
    Task task{sample_metadata()};
    REQUIRE(task.start_download().has_value());
    REQUIRE(task.begin_upload().has_value());
    REQUIRE(task.state() == TaskState::Uploading);
    REQUIRE(task.mark_completed().has_value());
    REQUIRE(task.state() == TaskState::Completed);
}

TEST_CASE("mark_failed records the reason and is reachable from any non-terminal state",
          "[domain][task]") {
    const auto state = GENERATE(
        TaskState::Queued, TaskState::Downloading, TaskState::Processing, TaskState::Uploading);
    Task task{sample_metadata()};
    drive_to(task, state);

    REQUIRE(task.mark_failed("download stalled").has_value());
    REQUIRE(task.state() == TaskState::Failed);
    REQUIRE(task.is_terminal());
    REQUIRE(task.error_message().has_value());
    REQUIRE(*task.error_message() == "download stalled");
}

TEST_CASE("mark_cancelled is reachable from any non-terminal state", "[domain][task]") {
    const auto state = GENERATE(
        TaskState::Queued, TaskState::Downloading, TaskState::Processing, TaskState::Uploading);
    Task task{sample_metadata()};
    drive_to(task, state);

    REQUIRE(task.mark_cancelled().has_value());
    REQUIRE(task.state() == TaskState::Cancelled);
    REQUIRE(task.is_terminal());
    REQUIRE_FALSE(task.error_message().has_value());
}

TEST_CASE("Invalid transitions yield InvalidState and preserve current state", "[domain][task]") {
    auto [start_state, action] = GENERATE(table<TaskState, Action>({
        // From Queued: cannot skip Downloading.
        {TaskState::Queued, k_begin_processing},
        {TaskState::Queued, k_begin_upload},
        {TaskState::Queued, k_mark_completed},

        // From Downloading: cannot complete, cannot re-download.
        {TaskState::Downloading, k_start_download},
        {TaskState::Downloading, k_mark_completed},

        // From Processing: cannot restart download, cannot reprocess, cannot complete.
        {TaskState::Processing, k_start_download},
        {TaskState::Processing, k_begin_processing},
        {TaskState::Processing, k_mark_completed},

        // From Uploading: cannot rewind to download/process, cannot re-upload.
        {TaskState::Uploading, k_start_download},
        {TaskState::Uploading, k_begin_processing},
        {TaskState::Uploading, k_begin_upload},

        // Terminal states reject every transition.
        {TaskState::Completed, k_start_download},
        {TaskState::Completed, k_begin_processing},
        {TaskState::Completed, k_begin_upload},
        {TaskState::Completed, k_mark_completed},
        {TaskState::Completed, k_mark_failed},
        {TaskState::Completed, k_mark_cancelled},

        {TaskState::Failed, k_start_download},
        {TaskState::Failed, k_begin_processing},
        {TaskState::Failed, k_begin_upload},
        {TaskState::Failed, k_mark_completed},
        {TaskState::Failed, k_mark_failed},
        {TaskState::Failed, k_mark_cancelled},

        {TaskState::Cancelled, k_start_download},
        {TaskState::Cancelled, k_begin_processing},
        {TaskState::Cancelled, k_begin_upload},
        {TaskState::Cancelled, k_mark_completed},
        {TaskState::Cancelled, k_mark_failed},
        {TaskState::Cancelled, k_mark_cancelled},
    }));

    Task task{sample_metadata()};
    drive_to(task, start_state);
    REQUIRE(task.state() == start_state);

    const auto result = action(task);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::InvalidState);
    REQUIRE(task.state() == start_state);
}

TEST_CASE("Valid transitions table", "[domain][task]") {
    auto [start_state, action, expected] = GENERATE(table<TaskState, Action, TaskState>({
        {TaskState::Queued, k_start_download, TaskState::Downloading},
        {TaskState::Downloading, k_begin_processing, TaskState::Processing},
        {TaskState::Downloading, k_begin_upload, TaskState::Uploading},
        {TaskState::Processing, k_begin_upload, TaskState::Uploading},
        {TaskState::Uploading, k_mark_completed, TaskState::Completed},
        {TaskState::Queued, k_mark_failed, TaskState::Failed},
        {TaskState::Downloading, k_mark_failed, TaskState::Failed},
        {TaskState::Processing, k_mark_failed, TaskState::Failed},
        {TaskState::Uploading, k_mark_failed, TaskState::Failed},
        {TaskState::Queued, k_mark_cancelled, TaskState::Cancelled},
        {TaskState::Downloading, k_mark_cancelled, TaskState::Cancelled},
        {TaskState::Processing, k_mark_cancelled, TaskState::Cancelled},
        {TaskState::Uploading, k_mark_cancelled, TaskState::Cancelled},
    }));

    Task task{sample_metadata()};
    drive_to(task, start_state);
    REQUIRE(task.state() == start_state);

    REQUIRE(action(task).has_value());
    REQUIRE(task.state() == expected);
}

TEST_CASE("metadata is preserved through transitions", "[domain][task]") {
    Task task{sample_metadata()};
    const auto original_id = task.metadata().id.value();
    const auto original_url = task.metadata().source_url;

    REQUIRE(task.start_download().has_value());
    REQUIRE(task.begin_upload().has_value());
    REQUIRE(task.mark_completed().has_value());

    REQUIRE(task.metadata().id.value() == original_id);
    REQUIRE(task.metadata().source_url == original_url);
}
