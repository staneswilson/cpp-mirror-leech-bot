#ifndef CMLB_UPLOADERS_RCLONE_UPLOADER_HPP
#define CMLB_UPLOADERS_RCLONE_UPLOADER_HPP

#include "uploaders/iuploader.hpp"
#include <atomic>
#include <thread>

namespace cmlb {

/**
 * @brief Rclone-based uploader for cloud storage.
 * 
 * Executes rclone as a subprocess and parses progress from stdout.
 * Supports all rclone remotes (GDrive, OneDrive, S3, etc.)
 */
class RcloneUploader : public IUploader {
public:
    struct Config {
        std::string rclone_path = "rclone";   // Path to rclone executable
        std::string config_path;               // Path to rclone.conf
        int transfers = 4;                     // Parallel transfers
        int checkers = 8;                      // Parallel checkers
    };

    explicit RcloneUploader(const Config& config);
    ~RcloneUploader() override;

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

    [[nodiscard]] std::string_view name() const noexcept override { return "rclone"; }
    [[nodiscard]] bool isReady() const noexcept override;

    /**
     * @brief List available remotes from rclone config.
     */
    Result<std::vector<std::string>> listRemotes();

private:
    Config config_;
    std::atomic<bool> cancelled_{false};
    
    // Parse rclone progress output
    static UploadProgress parseProgress(const std::string& line);
    
    // Execute rclone command
    Result<std::string> executeRclone(
        const std::vector<std::string>& args,
        UploadProgressCallback progress = nullptr
    );
};

} // namespace cmlb

#endif // CMLB_UPLOADERS_RCLONE_UPLOADER_HPP
