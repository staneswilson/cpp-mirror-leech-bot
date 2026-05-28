#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/media/archive_processor_interface.hpp>

namespace cmlb::infrastructure::system {
class Subprocess;
} // namespace cmlb::infrastructure::system

namespace cmlb::infrastructure::media {

/// `ArchiveProcessorInterface` adapter backed by the `7z` (p7zip) CLI.
/// 7-zip is preferred because it covers virtually every archive format
/// the bot is realistically asked to handle.
class SevenZipArchiveProcessor final : public ArchiveProcessorInterface {
public:
    explicit SevenZipArchiveProcessor(cmlb::infrastructure::system::Subprocess& subprocess,
                                      std::filesystem::path seven_zip_path = "7z");

    ~SevenZipArchiveProcessor() override = default;

    SevenZipArchiveProcessor(const SevenZipArchiveProcessor&) = delete;
    SevenZipArchiveProcessor& operator=(const SevenZipArchiveProcessor&) = delete;
    SevenZipArchiveProcessor(SevenZipArchiveProcessor&&) = delete;
    SevenZipArchiveProcessor& operator=(SevenZipArchiveProcessor&&) = delete;

    [[nodiscard]] bool can_handle(std::filesystem::path archive) const noexcept override;

    boost::asio::awaitable<cmlb::core::Result<std::vector<std::filesystem::path>>> extract(
        std::filesystem::path archive,
        std::filesystem::path output_dir,
        ArchiveExtractOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>> create_archive(
        std::filesystem::path output,
        std::vector<std::filesystem::path> inputs,
        ArchiveCreateOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<std::vector<std::string>>> list_contents(
        std::filesystem::path archive, std::optional<std::string> password) override;

    /// Resolved path to the configured `7z` binary.
    [[nodiscard]] const std::filesystem::path& seven_zip_path() const noexcept {
        return seven_zip_path_;
    }

private:
    cmlb::infrastructure::system::Subprocess* subprocess_;
    std::filesystem::path seven_zip_path_;
};

} // namespace cmlb::infrastructure::media
