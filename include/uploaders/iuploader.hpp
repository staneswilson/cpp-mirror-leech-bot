#ifndef CMLB_UPLOADERS_IUPLOADER_HPP
#define CMLB_UPLOADERS_IUPLOADER_HPP

#include "core/types.hpp"
#include <string>
#include <string_view>
#include <future>
#include <functional>
#include <filesystem>
#include <chrono>

namespace cmlb {

/**
 * @brief Upload progress information.
 */
struct UploadProgress {
    std::string file_name;
    int64_t total_bytes{0};
    int64_t uploaded_bytes{0};
    int64_t speed_bps{0};
    std::chrono::seconds eta{0};
    int current_part{0};
    int total_parts{1};
    
    [[nodiscard]] double progress() const noexcept {
        if (total_bytes == 0) return 0.0;
        return (static_cast<double>(uploaded_bytes) / static_cast<double>(total_bytes)) * 100.0;
    }
};

/**
 * @brief Result of a completed upload.
 */
struct UploadResult {
    bool success{false};
    std::string file_id;           // Telegram file_id or GDrive ID
    std::string link;              // Direct/shareable link if available
    std::string error_message;
    int64_t size{0};
    std::chrono::milliseconds duration{0};
};

/**
 * @brief Progress callback type.
 */
using UploadProgressCallback = std::function<void(const UploadProgress&)>;

/**
 * @brief Upload destination configuration.
 */
struct UploadConfig {
    std::optional<int64_t> chat_id;            // Telegram destination
    std::optional<std::string> folder_id;      // GDrive folder
    std::optional<std::string> rclone_path;    // Rclone remote:path
    
    bool as_document{false};                    // Force document upload
    std::optional<std::string> thumbnail_path;
    std::optional<std::string> caption;
    int64_t split_size{2'000'000'000};         // 2GB default
};

/**
 * @brief Abstract interface for upload destinations.
 */
class IUploader {
public:
    virtual ~IUploader() = default;

    /**
     * @brief Upload a single file.
     * @param path Local file path
     * @param config Upload configuration
     * @param progress Optional progress callback
     * @return Future with upload result
     */
    virtual std::future<Result<UploadResult>> uploadFile(
        const std::filesystem::path& path,
        const UploadConfig& config,
        UploadProgressCallback progress = nullptr
    ) = 0;

    /**
     * @brief Upload a directory (recursive).
     * @param path Local directory path
     * @param config Upload configuration
     * @param progress Optional progress callback
     * @return Future with upload results for each file
     */
    virtual std::future<Result<std::vector<UploadResult>>> uploadDirectory(
        const std::filesystem::path& path,
        const UploadConfig& config,
        UploadProgressCallback progress = nullptr
    ) = 0;

    /**
     * @brief Cancel an ongoing upload.
     * @param upload_id Identifier returned when upload started
     */
    virtual void cancelUpload(std::string_view upload_id) = 0;

    /**
     * @brief Get uploader name.
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Check if uploader is ready.
     */
    [[nodiscard]] virtual bool isReady() const noexcept = 0;
};

} // namespace cmlb

#endif // CMLB_UPLOADERS_IUPLOADER_HPP
