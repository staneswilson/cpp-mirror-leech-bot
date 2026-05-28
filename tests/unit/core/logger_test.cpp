#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmlb/core/logger.hpp>

using cmlb::core::LogConfig;
using cmlb::core::Logger;
using cmlb::core::parse_log_level;
using Catch::Matchers::ContainsSubstring;

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64 gen{rd()};
        for (int i = 0; i < 16; ++i) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("cmlb_log_" + std::to_string(gen()));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error{"could not create temp dir"};
    }
    ~TempDir() {
        // NOTE: do not call Logger::shutdown() here. The first TEST_CASE
        // already calls it; spdlog's global thread-pool destruction is not
        // idempotent in the version we use and double-shutdown SEGFAULTs on
        // Windows. Each test owns its own shutdown.
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&)                 = delete;
    TempDir& operator=(TempDir&&)      = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("parse_log_level recognizes the standard set", "[core][logger]") {
    CHECK(parse_log_level("trace").has_value());
    CHECK(parse_log_level("debug").has_value());
    CHECK(parse_log_level("info").has_value());
    CHECK(parse_log_level("warn").has_value());
    CHECK(parse_log_level("warning").has_value());
    CHECK(parse_log_level("error").has_value());
    CHECK(parse_log_level("critical").has_value());
    CHECK(parse_log_level("off").has_value());

    auto bad = parse_log_level("loud");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == cmlb::core::ErrorCode::InvalidArgument);
}

TEST_CASE("Logger writes to a rotating file sink and shuts down cleanly",
          "[core][logger]") {
    TempDir tmp;
    LogConfig cfg;
    cfg.logs_dir = tmp.path();
    cfg.level    = "debug";
    cfg.console  = false;  // keep test output clean
    cfg.rotating_file_max_bytes = 1024 * 1024;
    cfg.rotating_file_max_files = 2;

    auto init = Logger::initialize(cfg);
    REQUIRE(init.has_value());

    Logger::info("hello {}", "world");
    Logger::warn("warning code={}", 42);
    Logger::error("boom: {}", "fault");
    Logger::shutdown();

    const auto file = tmp.path() / "cmlb.log";
    REQUIRE(std::filesystem::exists(file));
    const auto contents = read_file(file);
    CHECK_THAT(contents, ContainsSubstring("hello world"));
    CHECK_THAT(contents, ContainsSubstring("warning code=42"));
    CHECK_THAT(contents, ContainsSubstring("boom: fault"));
}

TEST_CASE("Logger::initialize rejects unparseable levels", "[core][logger]") {
    TempDir tmp;
    LogConfig cfg;
    cfg.logs_dir = tmp.path();
    cfg.level    = "shouty";
    cfg.console  = false;
    auto init = Logger::initialize(cfg);
    REQUIRE_FALSE(init.has_value());
    CHECK(init.error().code == cmlb::core::ErrorCode::InvalidArgument);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
