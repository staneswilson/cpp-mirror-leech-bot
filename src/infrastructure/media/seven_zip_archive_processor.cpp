#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/media/seven_zip_archive_processor.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

namespace cmlb::infrastructure::media {

namespace {

namespace asio = boost::asio;
using cmlb::core::error;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;
using cmlb::infrastructure::system::Subprocess;
using cmlb::infrastructure::system::SubprocessRequest;
using cmlb::infrastructure::system::SubprocessResult;

/// Lowercases an ASCII string in-place.
[[nodiscard]] std::string ascii_lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

/// Set of archive extensions recognised by 7-zip. The double-suffix entries
/// (`.tar.gz`, etc.) must be matched against the *full* filename, not just
/// `path.extension()`, so we keep them separate.
constexpr std::array kSingleExtensions{
    ".zip",
    ".rar",
    ".7z",
    ".tar",
    ".gz",
    ".tgz",
    ".bz2",
    ".xz",
    ".iso",
};

constexpr std::array kCompoundSuffixes{
    ".tar.gz",
    ".tar.bz2",
    ".tar.xz",
};

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size()
           && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

asio::awaitable<Result<SubprocessResult>> run_subprocess(Subprocess& subprocess,
                                                         SubprocessRequest req,
                                                         ErrorCode failure_code) {
    auto result = co_await subprocess.run(std::move(req));
    if (!result) {
        co_return std::unexpected(result.error());
    }
    if (result->exit_code != 0) {
        Logger::warn(
            "[archive] 7z exited with code {}: {}", result->exit_code, result->stderr_data);
        co_return error(failure_code,
                        "7z exited with code " + std::to_string(result->exit_code) + ": "
                            + result->stderr_data);
    }
    co_return std::move(*result);
}

} // namespace

SevenZipArchiveProcessor::SevenZipArchiveProcessor(Subprocess& subprocess,
                                                   std::filesystem::path seven_zip_path)
    : subprocess_{&subprocess}, seven_zip_path_{std::move(seven_zip_path)} {
}

bool SevenZipArchiveProcessor::can_handle(std::filesystem::path archive) const noexcept {
    const auto name = ascii_lower(archive.filename().string());
    for (const auto& suf : kCompoundSuffixes) {
        if (ends_with(name, suf))
            return true;
    }
    for (const auto& ext : kSingleExtensions) {
        if (ends_with(name, ext))
            return true;
    }
    return false;
}

asio::awaitable<Result<std::vector<std::filesystem::path>>> SevenZipArchiveProcessor::extract(
    std::filesystem::path archive,
    std::filesystem::path output_dir,
    ArchiveExtractOptions options) {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        co_return error(ErrorCode::FileSystem,
                        "failed to create output directory: " + ec.message());
    }

    SubprocessRequest req{};
    req.executable = seven_zip_path_;
    req.arguments.emplace_back("x");
    if (options.overwrite_existing) {
        req.arguments.emplace_back("-aoa");
    } else {
        // `-aos` skips existing files; `-y` below answers "no" to overwrite
        // prompts. Pairing `-y` with `-aoa` is the standard "force" mode.
        req.arguments.emplace_back("-aos");
    }
    req.arguments.emplace_back("-y");
    req.arguments.emplace_back("-o" + output_dir.string());
    if (options.password) {
        req.arguments.emplace_back("-p" + *options.password);
    }
    req.arguments.emplace_back(archive.string());

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::ArchiveProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    // Best-effort enumeration of the destination directory.
    std::vector<std::filesystem::path> produced;
    for (auto it = std::filesystem::recursive_directory_iterator(
             output_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec)) {
            produced.emplace_back(it->path());
        }
    }
    co_return produced;
}

asio::awaitable<Result<std::filesystem::path>> SevenZipArchiveProcessor::create_archive(
    std::filesystem::path output,
    std::vector<std::filesystem::path> inputs,
    ArchiveCreateOptions options) {
    if (inputs.empty()) {
        co_return error(ErrorCode::InvalidArgument,
                        "create_archive requires at least one input path");
    }
    const auto level = std::clamp(options.compression_level, 0, 9);

    SubprocessRequest req{};
    req.executable = seven_zip_path_;
    req.arguments.emplace_back("a");
    req.arguments.emplace_back("-t7z");
    req.arguments.emplace_back("-y");
    if (options.password) {
        req.arguments.emplace_back("-p" + *options.password);
        if (options.encrypt_header) {
            req.arguments.emplace_back("-mhe=on");
        }
    }
    if (options.split_volume_size) {
        req.arguments.emplace_back("-v" + std::to_string(options.split_volume_size->bytes()) + "b");
    }
    req.arguments.emplace_back("-mx=" + std::to_string(level));
    req.arguments.emplace_back(output.string());
    for (const auto& input : inputs) {
        req.arguments.emplace_back(input.string());
    }

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::ArchiveProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }
    co_return output;
}

asio::awaitable<Result<std::vector<std::string>>> SevenZipArchiveProcessor::list_contents(
    std::filesystem::path archive, std::optional<std::string> password) {
    SubprocessRequest req{};
    req.executable = seven_zip_path_;
    req.arguments = {"l", "-slt", "-y"};
    if (password) {
        req.arguments.emplace_back("-p" + *password);
    }
    req.arguments.emplace_back(archive.string());

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::ArchiveProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    // Parse "Path = ..." lines. Skip the first one — `7z l -slt` echoes the
    // archive's own path as the first record. We detect that by tracking the
    // record boundary marker (blank lines separate records).
    std::vector<std::string> entries;
    std::string_view text{run_result->stdout_data};
    std::size_t pos = 0;
    bool seen_first_record = false;
    while (pos < text.size()) {
        const auto eol = text.find('\n', pos);
        const auto line_end = (eol == std::string_view::npos) ? text.size() : eol;
        std::string_view line = text.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        constexpr std::string_view kPrefix = "Path = ";
        if (line.starts_with(kPrefix)) {
            std::string value{line.substr(kPrefix.size())};
            if (!seen_first_record) {
                // First "Path = <archive>" entry — skip.
                seen_first_record = true;
            } else {
                entries.emplace_back(std::move(value));
            }
        }
        pos = line_end + 1;
    }
    co_return entries;
}

} // namespace cmlb::infrastructure::media
