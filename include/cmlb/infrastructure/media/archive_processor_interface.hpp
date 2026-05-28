#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/byte_size.hpp>

/// @file archive_processor_interface.hpp
/// @brief Abstract interface for archive extraction and creation. Concrete
///        implementations shell out to `7z`/`unar` and friends.

namespace cmlb::infrastructure::media {

/// Per-call extraction settings.
struct ArchiveExtractOptions {
    /// Optional password for encrypted archives.
    std::optional<std::string> password;
    /// When true, existing destination files are overwritten silently.
    bool overwrite_existing{false};
};

/// Per-call creation settings.
struct ArchiveCreateOptions {
    /// Optional password to encrypt the archive contents (and, optionally,
    /// the header — see @ref encrypt_header).
    std::optional<std::string> password;
    /// When true and a password is supplied, the file listing is encrypted
    /// as well (7-zip's `-mhe=on`).
    bool encrypt_header{false};
    /// Optional split-volume size. When supplied, the archive is produced
    /// in multiple volumes of this size.
    std::optional<cmlb::domain::ByteSize> split_volume_size;
    /// 7-zip compression level (0-9; 0 == store, 5 == default, 9 == ultra).
    int compression_level{5};
};

/// Polymorphic archive-processor seam.
class ArchiveProcessorInterface {
public:
    virtual ~ArchiveProcessorInterface() = default;

    ArchiveProcessorInterface()                                            = default;
    ArchiveProcessorInterface(const ArchiveProcessorInterface&)            = delete;
    ArchiveProcessorInterface& operator=(const ArchiveProcessorInterface&) = delete;
    ArchiveProcessorInterface(ArchiveProcessorInterface&&)                 = delete;
    ArchiveProcessorInterface& operator=(ArchiveProcessorInterface&&)      = delete;

    /// Cheap, side-effect-free predicate: does the implementation recognise
    /// @p archive's extension as something it can process?
    [[nodiscard]] virtual bool can_handle(std::filesystem::path archive) const noexcept = 0;

    /// Extracts @p archive into @p output_dir. Returns the absolute paths
    /// of the extracted files (best-effort: implementations that cannot
    /// enumerate may return only the top-level entries).
    virtual boost::asio::awaitable<cmlb::core::Result<std::vector<std::filesystem::path>>>
        extract(std::filesystem::path archive,
                std::filesystem::path output_dir,
                ArchiveExtractOptions options) = 0;

    /// Creates a new archive at @p output containing @p inputs.
    virtual boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        create_archive(std::filesystem::path output,
                       std::vector<std::filesystem::path> inputs,
                       ArchiveCreateOptions options) = 0;

    /// Enumerates the entries in @p archive without extracting them.
    virtual boost::asio::awaitable<cmlb::core::Result<std::vector<std::string>>>
        list_contents(std::filesystem::path archive,
                      std::optional<std::string> password) = 0;
};

}  // namespace cmlb::infrastructure::media
