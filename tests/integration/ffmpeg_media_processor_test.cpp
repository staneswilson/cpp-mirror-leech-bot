// ---------------------------------------------------------------------------
// tests/integration/ffmpeg_media_processor_test.cpp
//
// Live-binary integration check for FfmpegMediaProcessor. Runs only when
// both `ffmpeg` and `ffprobe` are resolvable on PATH; otherwise registers
// itself under the hidden `[.]` tag so it doesn't surface as a failure
// on developer machines without the toolchain installed.
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/infrastructure/media/ffmpeg_media_processor.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

namespace {

[[nodiscard]] bool tool_on_path(std::string_view name) {
#ifdef _WIN32
    const std::string cmd = "where " + std::string{name} + " >NUL 2>&1";
#else
    const std::string cmd = "command -v " + std::string{name} + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

} // namespace

TEST_CASE("FfmpegMediaProcessor probes a fixture file when ffmpeg is available",
          "[integration][media][ffmpeg][.]") {
    if (!tool_on_path("ffmpeg") || !tool_on_path("ffprobe")) {
        SUCCEED("ffmpeg/ffprobe not on PATH — skipping live integration test");
        return;
    }

    cmlb::core::Executor executor{2};
    // Subprocess takes an asio executor handle, not the core::Executor wrapper.
    cmlb::infrastructure::system::Subprocess subprocess{executor.get_executor()};
    cmlb::infrastructure::media::FfmpegMediaProcessor processor{subprocess};

    // Resolve fixture path from CMake-injected ${CMAKE_SOURCE_DIR}/tests/fixtures
    // if present; otherwise just confirm the processor reports an error on a
    // non-existent file (still useful as a smoke test of the error path).
    const std::filesystem::path fixture = "tests/fixtures/sample.mp4";
    if (!std::filesystem::exists(fixture)) {
        WARN("fixture " << fixture << " not present; only exercising error path");
        auto fut = boost::asio::co_spawn(
            executor.get_executor(),
            [&]() -> boost::asio::awaitable<bool> {
                auto r = co_await processor.probe("nonexistent.mp4");
                co_return !r.has_value();
            },
            boost::asio::use_future);
        CHECK(fut.get());
        return;
    }

    auto fut = boost::asio::co_spawn(
        executor.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            auto r = co_await processor.probe(fixture);
            co_return r.has_value() && r->duration.count() > 0;
        },
        boost::asio::use_future);
    CHECK(fut.get());
}
