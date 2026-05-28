#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>

namespace cmlb::infrastructure::system {

/// Fully describes a child process to be spawned by `Subprocess::run()`.
///
/// All fields except `executable` have sensible defaults; the most common
/// usage is `SubprocessRequest{ .executable = "ffprobe", .arguments = {...} }`.
struct SubprocessRequest {
    /// Program to run. If not an absolute path, PATH is searched.
    std::filesystem::path executable;
    /// Argument list (does *not* include argv[0]; the implementation prepends
    /// the executable name).
    std::vector<std::string> arguments;
    /// Environment variables to set or override on top of the parent's env.
    /// Variables not present here are inherited from the parent process.
    std::vector<std::pair<std::string, std::string>> environment;
    /// Initial working directory for the child. Empty inherits the parent's.
    std::filesystem::path working_directory;
    /// If true, stdout is captured into `SubprocessResult::stdout_data` and
    /// (if set) forwarded line-by-line to `on_stdout_line`.
    bool capture_stdout = true;
    /// Same as `capture_stdout` for the stderr stream.
    bool capture_stderr = true;
    /// Optional data piped to the child's stdin. The pipe is closed once the
    /// full buffer has been written, so the child sees EOF.
    std::optional<std::string> stdin_data;
    /// Hard deadline. On expiry the child is sent a graceful termination
    /// signal (`SIGTERM` / `TerminateProcess`); after a further 5 seconds it
    /// is killed (`SIGKILL`). `timed_out` is set in the result.
    std::chrono::milliseconds timeout{std::chrono::minutes(5)};
    /// If set, called with every complete stdout line as soon as it arrives.
    /// The newline (`\n` or `\r\n`) is stripped. Does not replace `stdout_data`
    /// capture. INVOKED FROM A WORKER THREAD — if the callback touches
    /// strand-bound state, post to the appropriate executor inside.
    std::function<void(std::string_view line)> on_stdout_line;
    /// Same as `on_stdout_line` for stderr. May fire concurrently with
    /// `on_stdout_line`; if both share state, the caller must synchronize.
    std::function<void(std::string_view line)> on_stderr_line;
};

/// Outcome of a child process invocation.
struct SubprocessResult {
    /// Numeric exit code reported by the OS. 0 conventionally means success.
    int exit_code = 0;
    /// Full captured stdout (empty if `capture_stdout` was false).
    std::string stdout_data;
    /// Full captured stderr (empty if `capture_stderr` was false).
    std::string stderr_data;
    /// Wall-clock duration from spawn to reap.
    std::chrono::milliseconds duration{0};
    /// True if the timeout elapsed and the child had to be killed.
    bool timed_out = false;
    /// True if the awaitable was cancelled by the caller and the child was
    /// terminated as a result. Distinguishable from `timed_out` so callers
    /// can surface a "Cancelled" vs "Timed out" message to the user.
    bool cancelled = false;
};

/// Asynchronous subprocess runner.
///
/// Wraps `boost::process` with cancellation and timeout handling tailored
/// to CMLB's needs (running `aria2c`, `ffmpeg`, `ffprobe`, `rclone`).
///
/// Cancellation: respects `boost::asio::this_coro::cancellation_state()`.
/// On cancellation the child is gracefully terminated, then killed if it
/// fails to exit within 5 seconds.
class Subprocess {
public:
    /// Creates a runner bound to `exec`. The executor must outlive every
    /// outstanding awaitable returned by this object.
    explicit Subprocess(boost::asio::any_io_executor exec);

    /// Spawns the process described by `request` and waits for it to exit.
    ///
    /// Failure modes:
    /// * `InvalidArgument` — `request.executable` is empty.
    /// * `NotFound` — executable could not be located on PATH.
    /// * `Io` — pipe creation or read/write failed.
    /// * `DeadlineExceeded` — timed_out=true *and* exit_code != 0 indicates
    ///   the child was forcefully killed.
    ///
    /// On a clean exit (even with non-zero `exit_code`) the function returns
    /// a populated `SubprocessResult`; callers are responsible for treating
    /// non-zero exits as errors when appropriate.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<SubprocessResult>>
        run(SubprocessRequest request);

    /// Runs `executable version_flag` and returns the trimmed first line of
    /// stdout. Convenience for startup self-checks (e.g. `aria2c --version`).
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::string>>
        version_of(std::filesystem::path executable,
                   std::string version_flag = "--version");

private:
    boost::asio::any_io_executor exec_;
};

}  // namespace cmlb::infrastructure::system
