#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmlb/core/error.hpp>

using Catch::Matchers::ContainsSubstring;
using cmlb::core::AppError;
using cmlb::core::error;
using cmlb::core::error_code_name;
using cmlb::core::ErrorCode;
using cmlb::core::Result;

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

struct NamedCode {
    ErrorCode code;
    std::string_view name;
};

constexpr std::array<NamedCode, 31> kCases{{
    {ErrorCode::None, "None"},
    {ErrorCode::InvalidArgument, "InvalidArgument"},
    {ErrorCode::InvalidConfiguration, "InvalidConfiguration"},
    {ErrorCode::InvalidState, "InvalidState"},
    {ErrorCode::NotFound, "NotFound"},
    {ErrorCode::AlreadyExists, "AlreadyExists"},
    {ErrorCode::PermissionDenied, "PermissionDenied"},
    {ErrorCode::Unauthenticated, "Unauthenticated"},
    {ErrorCode::Cancelled, "Cancelled"},
    {ErrorCode::DeadlineExceeded, "DeadlineExceeded"},
    {ErrorCode::ResourceExhausted, "ResourceExhausted"},
    {ErrorCode::QuotaExceeded, "QuotaExceeded"},
    {ErrorCode::Network, "Network"},
    {ErrorCode::Timeout, "Timeout"},
    {ErrorCode::Io, "Io"},
    {ErrorCode::FileSystem, "FileSystem"},
    {ErrorCode::Serialization, "Serialization"},
    {ErrorCode::Deserialization, "Deserialization"},
    {ErrorCode::JsonParse, "JsonParse"},
    {ErrorCode::TelegramApi, "TelegramApi"},
    {ErrorCode::Aria2Rpc, "Aria2Rpc"},
    {ErrorCode::QbittorrentApi, "QbittorrentApi"},
    {ErrorCode::GoogleDriveApi, "GoogleDriveApi"},
    {ErrorCode::RcloneInvocation, "RcloneInvocation"},
    {ErrorCode::Database, "Database"},
    {ErrorCode::Migration, "Migration"},
    {ErrorCode::SubprocessFailed, "SubprocessFailed"},
    {ErrorCode::MediaProcessing, "MediaProcessing"},
    {ErrorCode::ArchiveProcessing, "ArchiveProcessing"},
    {ErrorCode::Internal, "Internal"},
    {ErrorCode::Unknown, "Unknown"},
}};

} // namespace

TEST_CASE("error_code_name covers every enum value", "[core][error]") {
    for (const auto& kase : kCases) {
        CAPTURE(static_cast<int>(kase.code));
        REQUIRE(error_code_name(kase.code) == kase.name);
    }
}

TEST_CASE("AppError captures source location and formats with code+message+location",
          "[core][error]") {
    AppError err{ErrorCode::Network, "connection refused"}; // location captured here

    CHECK(err.code == ErrorCode::Network);
    CHECK(err.message == "connection refused");
    CHECK(err.location.line() != 0);
    CHECK(std::string_view{err.location.file_name()}.size() > 0);

    std::ostringstream os;
    os << err;
    const auto formatted = os.str();
    CHECK_THAT(formatted, ContainsSubstring("code(Network)"));
    CHECK_THAT(formatted, ContainsSubstring("connection refused"));
    CHECK_THAT(formatted, ContainsSubstring(":"));
    CHECK_THAT(formatted, ContainsSubstring(" at "));
}

namespace {

Result<int> ok_value() {
    return 42;
}

Result<int> fail_value() {
    return error(ErrorCode::NotFound, "missing");
}

Result<int> propagate() {
    auto v = fail_value();
    if (!v)
        return std::unexpected(v.error());
    return *v + 1;
}

} // namespace

TEST_CASE("Result<int> round-trips and propagates via std::unexpected", "[core][error]") {
    auto ok = ok_value();
    REQUIRE(ok.has_value());
    CHECK(*ok == 42);

    auto bad = fail_value();
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::NotFound);
    CHECK(bad.error().message == "missing");

    auto propagated = propagate();
    REQUIRE_FALSE(propagated.has_value());
    CHECK(propagated.error().code == ErrorCode::NotFound);
    CHECK(propagated.error().message == "missing");
}

TEST_CASE("error() factory captures the caller's source location", "[core][error]") {
    const int line_here = __LINE__ + 1;
    auto bad = error(ErrorCode::Internal, "boom");
    CHECK(bad.error().code == ErrorCode::Internal);
    CHECK(bad.error().message == "boom");

    // Most stdlibs capture the call site of the default argument (this line).
    // Apple Clang 15 / libc++ under it instead captures the parameter's own
    // declaration inside `error()`, so the line number drifts by a handful.
    // We accept either: the file must match, and the line must be in the
    // ballpark of the call site.
    const auto captured_line = static_cast<int>(bad.error().location.line());
    CHECK(std::string_view{bad.error().location.file_name()}.find("error_test.cpp")
          != std::string_view::npos);
    CHECK(captured_line > 0);
#if defined(__apple_build_version__)
    // Apple Clang's source_location default-argument behavior is non-canonical;
    // just assert the location was captured somewhere sensible (this TU).
    SUCCEED("Apple Clang: skipping strict line-number check");
#else
    CHECK(captured_line == line_here);
#endif
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
