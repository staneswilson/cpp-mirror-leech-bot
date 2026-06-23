// Unit tests for cmlb::infrastructure::system::Subprocess.
//
// We run a tiny platform-appropriate command and assert that exit_code is 0
// and that the captured stdout contains the expected token.

#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

using Catch::Matchers::ContainsSubstring;
using cmlb::infrastructure::system::Subprocess;
using cmlb::infrastructure::system::SubprocessRequest;
using cmlb::infrastructure::system::SubprocessResult;

namespace {

#if defined(_WIN32)
constexpr const char* kShellExe = "cmd.exe";
const std::vector<std::string> kEchoArgs{"/c", "echo", "hello-cmlb"};
#else
constexpr const char* kShellExe = "/bin/sh";
const std::vector<std::string> kEchoArgs{"-c", "echo hello-cmlb"};
#endif

cmlb::core::Result<SubprocessResult> run_with_watchdog(SubprocessRequest req,
                                                       std::chrono::milliseconds timeout) {
    boost::asio::io_context io;
    Subprocess sub{io.get_executor()};

    auto fut = boost::asio::co_spawn(
        io.get_executor(),
        [&]() -> boost::asio::awaitable<cmlb::core::Result<SubprocessResult>> {
            co_return co_await sub.run(std::move(req));
        },
        boost::asio::use_future);

    std::thread runner{[&io]() {
        io.run();
    }};

    if (fut.wait_for(timeout) != std::future_status::ready) {
        io.stop();
        runner.join();
        return cmlb::core::error(cmlb::core::ErrorCode::DeadlineExceeded,
                                 "Subprocess test timed out waiting for awaitable completion");
    }
    runner.join();
    return fut.get();
}

} // namespace

TEST_CASE("Subprocess returns InvalidArgument for an empty executable without blocking",
          "[infrastructure][system][subprocess]") {
    for (int attempt = 0; attempt < 32; ++attempt) {
        SubprocessRequest req;
        req.timeout = std::chrono::milliseconds{10};

        auto result = run_with_watchdog(std::move(req), std::chrono::seconds{2});

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == cmlb::core::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("Subprocess runs a trivial echo command", "[infrastructure][system][subprocess]") {
    boost::asio::io_context io;
    Subprocess sub{io.get_executor()};

    SubprocessRequest req;
    req.executable = kShellExe;
    req.arguments = kEchoArgs;
    req.timeout = std::chrono::seconds(10);

    auto fut = boost::asio::co_spawn(
        io.get_executor(),
        [&]() -> boost::asio::awaitable<cmlb::core::Result<SubprocessResult>> {
            co_return co_await sub.run(std::move(req));
        },
        boost::asio::use_future);

    io.run();

    auto result = fut.get();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);
    CHECK_FALSE(result->timed_out);
    CHECK_THAT(result->stdout_data, ContainsSubstring("hello-cmlb"));
}

TEST_CASE("Subprocess returns NotFound for missing executables",
          "[infrastructure][system][subprocess]") {
    boost::asio::io_context io;
    Subprocess sub{io.get_executor()};

    SubprocessRequest req;
    req.executable = "this-binary-definitely-does-not-exist-cmlb";

    auto fut = boost::asio::co_spawn(
        io.get_executor(),
        [&]() -> boost::asio::awaitable<cmlb::core::Result<SubprocessResult>> {
            co_return co_await sub.run(std::move(req));
        },
        boost::asio::use_future);

    io.run();
    auto result = fut.get();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == cmlb::core::ErrorCode::NotFound);
}

TEST_CASE("Subprocess on_stdout_line fires once per newline-terminated line",
          "[infrastructure][system][subprocess]") {
    boost::asio::io_context io;
    Subprocess sub{io.get_executor()};

    std::mutex mu;
    std::vector<std::string> lines;

    SubprocessRequest req;
    req.executable = kShellExe;
#if defined(_WIN32)
    // cmd.exe joins with `&`. Echo on Windows emits `\r\n` — the LineSlicer
    // is expected to strip the trailing `\r` before delivering to the
    // callback, so we assert the bare tokens.
    req.arguments = {"/c", "echo line-a&echo line-b&echo line-c"};
#else
    req.arguments = {"-c", "printf 'line-a\\nline-b\\nline-c\\n'"};
#endif
    req.timeout = std::chrono::seconds(10);
    req.on_stdout_line = [&](std::string_view line) {
        std::lock_guard lk{mu};
        lines.emplace_back(line);
    };

    auto fut = boost::asio::co_spawn(
        io.get_executor(),
        [&]() -> boost::asio::awaitable<cmlb::core::Result<SubprocessResult>> {
            co_return co_await sub.run(std::move(req));
        },
        boost::asio::use_future);

    io.run();
    auto result = fut.get();
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);

    std::lock_guard lk{mu};
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "line-a");
    CHECK(lines[1] == "line-b");
    CHECK(lines[2] == "line-c");

    // The full buffer is still populated alongside the per-line callback.
    CHECK_THAT(result->stdout_data, ContainsSubstring("line-a"));
    CHECK_THAT(result->stdout_data, ContainsSubstring("line-c"));
}
