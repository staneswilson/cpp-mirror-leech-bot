// SPDX-License-Identifier: MIT
//
// subprocess.cpp — portable subprocess runner.
//
// Spawns a child process and captures stdout/stderr without depending on
// Boost.Process (whose v2 API churns between releases). Uses native OS
// primitives:
//
//   * POSIX (Linux / macOS) — fork + execvp + waitpid + pipe + read
//   * Win32                 — CreateProcessW + CreatePipe + ReadFile + WaitForSingleObject
//
// Async surface: the synchronous spawn runs on a worker `std::thread`;
// the calling coroutine polls a completion flag with a steady_timer.
// Cancellation of the awaitable triggers a graceful child
// termination (SIGTERM / TerminateProcess) followed by a hard kill after
// 5 seconds if the child fails to exit.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

#if defined(_WIN32)
// clang-format off
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// clang-format on
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <functional>
#include <string_view>

namespace cmlb::infrastructure::system {

namespace {

namespace asio = boost::asio;

constexpr auto kWorkerPollInterval = std::chrono::milliseconds{10};

// Incremental line-mode buffer.
//
// Bytes arrive in arbitrary chunks (TCP / pipe semantics — not aligned to
// line boundaries). For each chunk we (a) optionally append to a capture
// string for the post-mortem `SubprocessResult`, (b) split at '\n', emit
// each complete line through the caller's callback, and (c) carry the
// trailing partial line into the next chunk. `\r\n` is treated like `\n`
// so Windows tools (rclone on Windows, 7z) don't leak the CR.
//
// THREAD SAFETY: a `LineSlicer` is owned by one reader (one pipe). The
// caller's `on_line` callback may be invoked from a worker thread — if it
// touches coroutine/strand state, the caller is responsible for posting.
struct LineSlicer {
    std::string* full; // points into SubprocessResult; nullptr ⇒ no capture
    std::string pending;
    std::function<void(std::string_view)> on_line;

    void feed(std::string_view chunk) {
        if (full != nullptr)
            full->append(chunk);
        if (!on_line)
            return;
        pending.append(chunk);
        std::size_t pos = 0;
        while (true) {
            const auto nl = pending.find('\n', pos);
            if (nl == std::string::npos)
                break;
            std::size_t end = nl;
            if (end > pos && pending[end - 1] == '\r')
                --end;
            on_line(std::string_view{pending.data() + pos, end - pos});
            pos = nl + 1;
        }
        if (pos > 0)
            pending.erase(0, pos);
    }

    void flush() {
        if (!on_line || pending.empty())
            return;
        std::size_t end = pending.size();
        if (end > 0 && pending[end - 1] == '\r')
            --end;
        if (end > 0) {
            on_line(std::string_view{pending.data(), end});
        }
        pending.clear();
    }
};

// External cancellation channel for the in-flight child. `Subprocess::run`
// creates one of these, passes it to the worker thread, and installs an
// asio cancellation handler that calls `kill_now()` when the caller cancels
// the awaitable. The worker stores the native process handle (HANDLE on
// Windows, pid_t on POSIX) as soon as the child has been spawned.
struct CancelHandle {
#if defined(_WIN32)
    std::atomic<std::intptr_t> handle{0}; // HANDLE; 0 = no child yet
#else
    std::atomic<int> pid{0}; // pid_t; 0 = no child yet
#endif
    std::atomic<bool> cancelled{false};

    void kill_now() noexcept {
        cancelled.store(true, std::memory_order_release);
#if defined(_WIN32)
        const auto raw = handle.load(std::memory_order_acquire);
        if (raw != 0) {
            // TerminateProcess is the only reliable way on Windows; there is
            // no "polite" signal. Exit code 1 matches the timeout path.
            ::TerminateProcess(reinterpret_cast<HANDLE>(raw), 1);
        }
#else
        const auto p = pid.load(std::memory_order_acquire);
        if (p > 0) {
            // Polite SIGTERM first — the worker's wait loop observes the
            // exit and reaps the child. The 5s grace + SIGKILL escalation
            // is owned by the worker thread already, so we don't double up
            // from this signal handler context.
            ::kill(p, SIGTERM);
        }
#endif
    }
};

#if defined(_WIN32)

// Quote one argument according to Windows CommandLineToArgvW rules.
[[nodiscard]] std::wstring quote_arg_w(const std::string& arg) {
    std::wstring out;
    out.reserve(arg.size() + 2);
    const bool needs_quote = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
    if (needs_quote)
        out.push_back(L'"');
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            for (int i = 0; i < backslashes * 2 + 1; ++i)
                out.push_back(L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            for (int i = 0; i < backslashes; ++i)
                out.push_back(L'\\');
            backslashes = 0;
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
    }
    if (needs_quote) {
        for (int i = 0; i < backslashes * 2; ++i)
            out.push_back(L'\\');
        out.push_back(L'"');
    } else {
        for (int i = 0; i < backslashes; ++i)
            out.push_back(L'\\');
    }
    return out;
}

[[nodiscard]] cmlb::core::Result<SubprocessResult> spawn_sync(const SubprocessRequest& req,
                                                              CancelHandle* cancel_handle) {
    SubprocessResult result;
    const auto start = std::chrono::steady_clock::now();

    if (req.executable.empty()) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                 "Subprocess: empty executable path");
    }

    std::wstring cmdline = quote_arg_w(req.executable.string());
    for (const auto& arg : req.arguments) {
        cmdline.push_back(L' ');
        cmdline += quote_arg_w(arg);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_r = nullptr, stdin_w = nullptr;
    HANDLE stdout_r = nullptr, stdout_w = nullptr;
    HANDLE stderr_r = nullptr, stderr_w = nullptr;

    auto close_h = [](HANDLE& h) {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    };

    if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0) || !CreatePipe(&stdout_r, &stdout_w, &sa, 0)
        || !CreatePipe(&stderr_r, &stderr_w, &sa, 0)) {
        return cmlb::core::error(cmlb::core::ErrorCode::Io, "Subprocess: CreatePipe failed");
    }

    SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_r;
    si.hStdOutput = stdout_w;
    si.hStdError = stderr_w;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    const wchar_t* working_dir = nullptr;
    std::wstring working_dir_w;
    if (!req.working_directory.empty()) {
        working_dir_w = req.working_directory.wstring();
        working_dir = working_dir_w.c_str();
    }

    const BOOL ok = CreateProcessW(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, working_dir, &si, &pi);

    close_h(stdin_r);
    close_h(stdout_w);
    close_h(stderr_w);

    if (!ok) {
        const DWORD err = GetLastError();
        close_h(stdin_w);
        close_h(stdout_r);
        close_h(stderr_r);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                     "Subprocess: executable not found: "
                                         + req.executable.string());
        }
        return cmlb::core::error(cmlb::core::ErrorCode::SubprocessFailed,
                                 "Subprocess: CreateProcessW failed (code " + std::to_string(err)
                                     + ")");
    }

    // Publish the process handle so an external cancellation can kill us
    // mid-run. Must happen BEFORE writing stdin / waiting on the child.
    if (cancel_handle != nullptr) {
        cancel_handle->handle.store(reinterpret_cast<std::intptr_t>(pi.hProcess),
                                    std::memory_order_release);
        if (cancel_handle->cancelled.load(std::memory_order_acquire)) {
            // Lost the race — the awaitable was already cancelled.
            ::TerminateProcess(pi.hProcess, 1);
        }
    }

    if (req.stdin_data) {
        DWORD written = 0;
        WriteFile(stdin_w,
                  req.stdin_data->data(),
                  static_cast<DWORD>(req.stdin_data->size()),
                  &written,
                  nullptr);
    }
    close_h(stdin_w);

    // Concurrent reader threads — one per pipe. ReadFile on anonymous pipes
    // is blocking and not OVERLAPPED-capable, so we let each thread block on
    // its own pipe. Both pipes are drained in parallel, avoiding the classic
    // deadlock where a chatty stderr fills its 64 KiB pipe buffer while the
    // parent is still reading stdout to EOF.
    // The readers are joined explicitly below — a thrown exception between
    // here and the join calls would std::terminate on the unjoined thread,
    // but the intervening code is noexcept-by-construction (handle ops only).
    auto reader = [](HANDLE h, LineSlicer slicer) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            slicer.feed(std::string_view{buf, static_cast<std::size_t>(n)});
        }
        slicer.flush();
    };

    LineSlicer stdout_slicer{
        req.capture_stdout ? &result.stdout_data : nullptr, {}, req.on_stdout_line};
    LineSlicer stderr_slicer{
        req.capture_stderr ? &result.stderr_data : nullptr, {}, req.on_stderr_line};

    std::thread stdout_reader{reader, stdout_r, std::move(stdout_slicer)};
    std::thread stderr_reader{reader, stderr_r, std::move(stderr_slicer)};

    const DWORD timeout_ms = static_cast<DWORD>(req.timeout.count());
    const DWORD wait_res = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_res == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    if (cancel_handle != nullptr && cancel_handle->cancelled.load(std::memory_order_acquire)) {
        result.cancelled = true;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    // Detach the handle from the cancel channel before we close it — any
    // late cancellation that races us here must not touch a closed handle.
    if (cancel_handle != nullptr) {
        cancel_handle->handle.store(0, std::memory_order_release);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Child is dead — its write handles to the pipes are closed by the OS,
    // so any pending ReadFile in our reader threads returns false. Joining
    // happens automatically via jthread's destructor below, but we close
    // the handles explicitly so the next `close_h` calls are no-ops.
    stdout_reader.join();
    stderr_reader.join();
    close_h(stdout_r);
    close_h(stderr_r);

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

#else // POSIX

[[nodiscard]] cmlb::core::Result<SubprocessResult> spawn_sync(const SubprocessRequest& req,
                                                              CancelHandle* cancel_handle) {
    SubprocessResult result;
    const auto start = std::chrono::steady_clock::now();

    if (req.executable.empty()) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                 "Subprocess: empty executable path");
    }

    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};

    auto close_fd = [](int& fd) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    };
    auto close_pipe = [&](int (&p)[2]) {
        close_fd(p[0]);
        close_fd(p[1]);
    };

    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return cmlb::core::error(cmlb::core::ErrorCode::Io, "Subprocess: pipe() failed");
    }

    std::vector<std::string> argv_storage;
    argv_storage.reserve(req.arguments.size() + 1);
    argv_storage.push_back(req.executable.string());
    for (const auto& a : req.arguments)
        argv_storage.push_back(a);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage)
        argv_ptrs.push_back(s.data());
    argv_ptrs.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
        return cmlb::core::error(cmlb::core::ErrorCode::SubprocessFailed,
                                 "Subprocess: fork() failed");
    }

    // Publish the pid to the cancel channel right after fork succeeds (in
    // the parent only). Late-cancellation race: if the awaitable was
    // cancelled *before* the worker observed it, kill the freshly-spawned
    // child here.
    if (pid > 0 && cancel_handle != nullptr) {
        cancel_handle->pid.store(pid, std::memory_order_release);
        if (cancel_handle->cancelled.load(std::memory_order_acquire)) {
            ::kill(pid, SIGTERM);
        }
    }

    if (pid == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);

        if (!req.working_directory.empty()) {
            if (::chdir(req.working_directory.c_str()) != 0) {
                ::_exit(127);
            }
        }
        ::execvp(req.executable.c_str(), argv_ptrs.data());
        ::_exit(127);
    }

    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);

    if (req.stdin_data) {
        const auto& d = *req.stdin_data;
        std::size_t off = 0;
        while (off < d.size()) {
            const ssize_t n = ::write(stdin_pipe[1], d.data() + off, d.size() - off);
            if (n < 0)
                break;
            off += static_cast<std::size_t>(n);
        }
    }
    close_fd(stdin_pipe[1]);

    // Concurrent drain of stdout + stderr via poll(2). Reading them
    // sequentially can deadlock: a chatty child fills the 64 KiB stderr pipe
    // buffer and blocks on write() while we're still draining stdout. Both
    // pipes are set non-blocking so a partial read never stalls the loop.
    LineSlicer stdout_slicer{
        req.capture_stdout ? &result.stdout_data : nullptr, {}, req.on_stdout_line};
    LineSlicer stderr_slicer{
        req.capture_stderr ? &result.stderr_data : nullptr, {}, req.on_stderr_line};

    const int stdout_fd = stdout_pipe[0];
    const int stderr_fd = stderr_pipe[0];
    ::fcntl(stdout_fd, F_SETFL, ::fcntl(stdout_fd, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(stderr_fd, F_SETFL, ::fcntl(stderr_fd, F_GETFL, 0) | O_NONBLOCK);

    bool stdout_open = true;
    bool stderr_open = true;
    const auto deadline = start + req.timeout;

    auto drain_one = [](int fd, LineSlicer& slicer) -> bool {
        // Returns true if pipe still open, false on EOF / unrecoverable error.
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                slicer.feed(std::string_view{buf, static_cast<std::size_t>(n)});
                continue;
            }
            if (n == 0) {
                slicer.flush();
                return false; // EOF
            }
#if EAGAIN == EWOULDBLOCK
            if (errno == EAGAIN)
                return true;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
#endif
            if (errno == EINTR)
                continue;
            slicer.flush();
            return false;
        }
    };

    int status = 0;
    bool reaped = false;
    while (stdout_open || stderr_open) {
        ::pollfd fds[2];
        int nfds = 0;
        if (stdout_open) {
            fds[nfds++] = {stdout_fd, POLLIN, 0};
        }
        if (stderr_open) {
            fds[nfds++] = {stderr_fd, POLLIN, 0};
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            ::kill(pid, SIGTERM);
            break;
        }
        // External cancellation — break out of the drain loop and let the
        // wait+SIGKILL escalation below handle process reaping. The
        // CancelHandle::kill_now() that fired this already sent SIGTERM.
        if (cancel_handle != nullptr && cancel_handle->cancelled.load(std::memory_order_acquire)) {
            result.cancelled = true;
            break;
        }
        const auto remain =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int poll_ms = static_cast<int>(std::min<long long>(remain, 1000));

        const int rc = ::poll(fds, static_cast<nfds_t>(nfds), poll_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0)
            continue; // round timeout — loop re-checks deadline

        for (int i = 0; i < nfds; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            if (fds[i].fd == stdout_fd) {
                if (!drain_one(stdout_fd, stdout_slicer))
                    stdout_open = false;
            } else if (fds[i].fd == stderr_fd) {
                if (!drain_one(stderr_fd, stderr_slicer))
                    stderr_open = false;
            }
        }
    }

    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);

    // After EOF on both pipes the child is normally a heartbeat away from
    // exiting. Tight 1ms WNOHANG poll until it reaps (was 10ms in the old
    // path — that latency dominated short-lived subprocess tests).
    if (!result.timed_out && !result.cancelled) {
        while (true) {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                break;
            }
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                if (cancel_handle != nullptr) {
                    cancel_handle->pid.store(0, std::memory_order_release);
                }
                return cmlb::core::error(cmlb::core::ErrorCode::SubprocessFailed,
                                         "Subprocess: waitpid failed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                ::kill(pid, SIGTERM);
                break;
            }
            if (cancel_handle != nullptr
                && cancel_handle->cancelled.load(std::memory_order_acquire)) {
                result.cancelled = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Timeout OR cancellation path: grant 5s grace, then SIGKILL. Both share
    // the same escalation since either way the child was sent SIGTERM and
    // must be reaped before we can return.
    if ((result.timed_out || result.cancelled) && !reaped) {
        const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < hard_deadline) {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                break;
            }
            if (r < 0 && errno != EINTR)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
    }

    // Detach pid before returning — symmetric to the Win32 path. Any late
    // cancellation observed after this point becomes a no-op rather than
    // signalling an unrelated process that may have reused the pid.
    if (cancel_handle != nullptr) {
        cancel_handle->pid.store(0, std::memory_order_release);
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }

    if (result.exit_code == 127 && !result.timed_out) {
        return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                 "Subprocess: executable not found: " + req.executable.string());
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

#endif

} // namespace

Subprocess::Subprocess(boost::asio::any_io_executor exec) : exec_{std::move(exec)} {
}

boost::asio::awaitable<cmlb::core::Result<SubprocessResult>> Subprocess::run(
    SubprocessRequest request) {
    auto exec = co_await asio::this_coro::executor;

    auto result_holder = std::make_shared<cmlb::core::Result<SubprocessResult>>(
        cmlb::core::error(cmlb::core::ErrorCode::Internal, "Subprocess: worker did not run"));
    auto worker_done = std::make_shared<std::atomic<bool>>(false);

    // Shared cancel channel: the worker publishes the native child handle
    // into it after spawn, and the asio cancellation handler installed below
    // calls `kill_now()` to propagate `/cancel` and timeout-from-caller into
    // the running child. shared_ptr because the worker thread and the
    // cancellation handler can race against the coroutine returning.
    auto cancel_handle = std::make_shared<CancelHandle>();

    std::thread worker{[req = std::move(request), result_holder, worker_done, cancel_handle]() {
        try {
            *result_holder = spawn_sync(req, cancel_handle.get());
        } catch (const std::exception& ex) {
            *result_holder = cmlb::core::error(
                cmlb::core::ErrorCode::Internal,
                std::string{"Subprocess: worker failed unexpectedly: "} + ex.what());
        } catch (...) {
            *result_holder = cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                               "Subprocess: worker failed unexpectedly");
        }
        worker_done->store(true, std::memory_order_release);
    }};

    // Hook the caller's cancellation slot — `/cancel <task>` and
    // `with_timeout(...)` both cancel the awaitable; we map that into a
    // signal to the child.
    const auto cs = co_await asio::this_coro::cancellation_state;
    if (cs.slot().is_connected()) {
        cs.slot().assign([cancel_handle](asio::cancellation_type /*type*/) noexcept {
            cancel_handle->kill_now();
        });
    }

    asio::steady_timer ready{exec};
    while (!worker_done->load(std::memory_order_acquire)) {
        ready.expires_after(kWorkerPollInterval);
        boost::system::error_code ec;
        co_await ready.async_wait(asio::bind_cancellation_slot(
            asio::cancellation_slot{}, asio::redirect_error(asio::use_awaitable, ec)));
    }

    // Clear the cancellation handler before the worker thread is joined so
    // that a slot fired after this point can't reach into a destroyed
    // closure (the handle itself stays alive via shared_ptr until both the
    // worker and any in-flight signal release their copies).
    if (cs.slot().is_connected()) {
        cs.slot().clear();
    }

    worker.join();

    co_return std::move(*result_holder);
}

boost::asio::awaitable<cmlb::core::Result<std::string>> Subprocess::version_of(
    std::filesystem::path executable, std::string version_flag) {
    SubprocessRequest req;
    req.executable = std::move(executable);
    req.arguments = {std::move(version_flag)};
    req.timeout = std::chrono::seconds{10};

    auto result = co_await run(std::move(req));
    if (!result)
        co_return std::unexpected{result.error()};

    std::string out = std::move(result->stdout_data);
    if (auto eol = out.find_first_of("\r\n"); eol != std::string::npos) {
        out.resize(eol);
    }
    co_return out;
}

} // namespace cmlb::infrastructure::system
