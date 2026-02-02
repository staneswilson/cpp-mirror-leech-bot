#ifndef CMLB_UPLOADERS_TELEGRAM_UPLOADER_HPP
#define CMLB_UPLOADERS_TELEGRAM_UPLOADER_HPP

#include "uploaders/iuploader.hpp"
#include <td/telegram/Client.h>
#include <memory>
#include <atomic>

namespace cmlb {

/**
 * @brief Telegram upload handler using Tdlib.
 * 
 * Features:
 * - Automatic file type detection (photo/video/audio/document)
 * - Large file splitting (>2GB)
 * - Progress tracking via Tdlib callbacks
 * - Thumbnail support
 */
class TelegramUploader : public IUploader {
public:
    /**
     * @brief Construct uploader with Tdlib client reference.
     * @param client_manager Shared Tdlib client manager
     * @param client_id Tdlib client ID
     */
    TelegramUploader(
        std::shared_ptr<td::ClientManager> client_manager,
        std::int32_t client_id
    );

    ~TelegramUploader() override;

    std::future<Result<UploadResult>> uploadFile(
        const std::filesystem::path& path,
        const UploadConfig& config,
        UploadProgressCallback progress = nullptr
    ) override;

    std::future<Result<std::vector<UploadResult>>> uploadDirectory(
        const std::filesystem::path& path,
        const UploadConfig& config,
        UploadProgressCallback progress = nullptr
    ) override;

    void cancelUpload(std::string_view upload_id) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "telegram"; }
    [[nodiscard]] bool isReady() const noexcept override { return ready_.load(); }

    /**
     * @brief Split a large file into parts.
     * @param path Source file path
     * @param part_size Maximum part size in bytes
     * @return Paths to split parts
     */
    static Result<std::vector<std::filesystem::path>> splitFile(
        const std::filesystem::path& path,
        int64_t part_size
    );

private:
    std::shared_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_;
    std::atomic<bool> ready_{true};
    
    // Determine content type from file extension
    enum class ContentType { Photo, Video, Audio, Document };
    static ContentType detectContentType(const std::filesystem::path& path);
};

} // namespace cmlb

#endif // CMLB_UPLOADERS_TELEGRAM_UPLOADER_HPP
