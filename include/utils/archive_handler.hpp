#ifndef CMLB_UTILS_ARCHIVE_HANDLER_HPP
#define CMLB_UTILS_ARCHIVE_HANDLER_HPP

#include "core/types.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace cmlb {

/**
 * @brief Archive extraction progress.
 */
struct ArchiveProgress {
    std::string current_file;
    int total_files{0};
    int extracted_files{0};
    int64_t total_bytes{0};
    int64_t extracted_bytes{0};
};

using ArchiveProgressCallback = std::function<void(const ArchiveProgress&)>;

/**
 * @brief Archive handler for extraction and compression.
 * 
 * Supports:
 * - ZIP, RAR, 7z, TAR, GZIP, BZIP2, XZ
 * - Password-protected archives
 * - Split file joining
 */
class ArchiveHandler {
public:
    struct Config {
        std::string sevenz_path = "7z";       // Path to 7z executable
        bool delete_after_extract{false};
        bool delete_after_archive{false};
    };

    explicit ArchiveHandler(const Config& config = {});

    /**
     * @brief Check if file is an archive.
     */
    static bool isArchive(const std::filesystem::path& path);

    /**
     * @brief Get archive type from extension.
     */
    static std::string getArchiveType(const std::filesystem::path& path);

    /**
     * @brief Extract an archive.
     * @param archive_path Path to archive
     * @param dest_path Destination directory
     * @param password Optional password
     * @param progress Progress callback
     * @return Extracted files list
     */
    Result<std::vector<std::filesystem::path>> extract(
        const std::filesystem::path& archive_path,
        const std::filesystem::path& dest_path,
        const std::optional<std::string>& password = std::nullopt,
        ArchiveProgressCallback progress = nullptr
    );

    /**
     * @brief Create an archive.
     * @param source_path File or directory to archive
     * @param archive_path Output archive path
     * @param password Optional password
     * @param split_size Split size (0 for no split)
     * @param progress Progress callback
     * @return Created archive path(s)
     */
    Result<std::vector<std::filesystem::path>> archive(
        const std::filesystem::path& source_path,
        const std::filesystem::path& archive_path,
        const std::optional<std::string>& password = std::nullopt,
        int64_t split_size = 0,
        ArchiveProgressCallback progress = nullptr
    );

    /**
     * @brief Join split archive files.
     * @param first_part Path to first part (.001)
     * @param output_path Output file path
     * @return Joined file path
     */
    Result<std::filesystem::path> joinSplitFiles(
        const std::filesystem::path& first_part,
        const std::filesystem::path& output_path
    );

    /**
     * @brief List contents of an archive.
     */
    Result<std::vector<std::string>> listContents(
        const std::filesystem::path& archive_path,
        const std::optional<std::string>& password = std::nullopt
    );

private:
    Config config_;
    
    Result<std::string> execute7z(const std::vector<std::string>& args);
};

} // namespace cmlb

#endif // CMLB_UTILS_ARCHIVE_HANDLER_HPP
