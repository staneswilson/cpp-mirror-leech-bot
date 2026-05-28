// ---------------------------------------------------------------------------
// tests/integration/seven_zip_archive_processor_test.cpp
//
// Live-binary integration check for SevenZipArchiveProcessor. Runs only
// when `7z` is resolvable on PATH; otherwise registers itself under the
// hidden `[.]` tag.
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/infrastructure/media/seven_zip_archive_processor.hpp>
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

TEST_CASE("SevenZipArchiveProcessor extension recognition", "[integration][media][7z]") {
    cmlb::core::Executor executor{1};
    cmlb::infrastructure::system::Subprocess subprocess{executor.get_executor()};
    cmlb::infrastructure::media::SevenZipArchiveProcessor processor{subprocess};

    CHECK(processor.can_handle("foo.zip"));
    CHECK(processor.can_handle("foo.tar.gz"));
    CHECK(processor.can_handle("foo.7z"));
    CHECK(processor.can_handle("FOO.RAR"));
    CHECK(processor.can_handle("foo.tar.xz"));
    CHECK_FALSE(processor.can_handle("foo.txt"));
    CHECK_FALSE(processor.can_handle("noext"));
}

TEST_CASE("SevenZipArchiveProcessor round-trips an archive when 7z is available",
          "[integration][media][7z][.]") {
    if (!tool_on_path("7z")) {
        SUCCEED("7z not on PATH — skipping live integration test");
        return;
    }

    cmlb::core::Executor executor{2};
    cmlb::infrastructure::system::Subprocess subprocess{executor.get_executor()};
    cmlb::infrastructure::media::SevenZipArchiveProcessor processor{subprocess};

    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path() / "cmlb_7z_roundtrip";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const auto input = tmp / "hello.txt";
    const auto output = tmp / "out.7z";
    const auto extract_dir = tmp / "extracted";

    {
        std::ofstream s{input};
        s << "hello, world\n";
    }

    auto create_fut = boost::asio::co_spawn(
        executor.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            cmlb::infrastructure::media::ArchiveCreateOptions opts{};
            auto r = co_await processor.create_archive(output, {input}, opts);
            co_return r.has_value();
        },
        boost::asio::use_future);
    REQUIRE(create_fut.get());
    REQUIRE(fs::exists(output));

    auto list_fut = boost::asio::co_spawn(
        executor.get_executor(),
        [&]() -> boost::asio::awaitable<std::size_t> {
            auto r = co_await processor.list_contents(output, std::nullopt);
            co_return r ? r->size() : std::size_t{0};
        },
        boost::asio::use_future);
    CHECK(list_fut.get() == 1);

    auto extract_fut = boost::asio::co_spawn(
        executor.get_executor(),
        [&]() -> boost::asio::awaitable<bool> {
            cmlb::infrastructure::media::ArchiveExtractOptions opts{};
            opts.overwrite_existing = true;
            auto r = co_await processor.extract(output, extract_dir, opts);
            co_return r.has_value() && !r->empty();
        },
        boost::asio::use_future);
    CHECK(extract_fut.get());
    CHECK(fs::exists(extract_dir / "hello.txt"));

    fs::remove_all(tmp);
}
