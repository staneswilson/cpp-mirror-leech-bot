#ifndef CMLB_DOWNLOADERS_IDOWNLOADER_HPP
#define CMLB_DOWNLOADERS_IDOWNLOADER_HPP

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <future>
#include <chrono>
#include <optional>
#include <vector>

namespace cmlb {

/**
 * @brief Download task state enumeration.
 */
enum class DownloadState {
    Waiting,        // Queued, not yet started
    Downloading,    // Actively downloading
    Paused,         // User paused
    Complete,       // Download finished successfully
    Error,          // Download failed
    Seeding,        // Torrent seeding (post-download)
    Removed         // Task was removed
};

/**
 * @brief Detailed status of a download task.
 */
struct DownloadStatus {
    std::string id;                     // Unique identifier (GID for aria2, hash for qBittorrent)
    std::string name;                   // File/torrent name
    DownloadState state;
    
    int64_t total_bytes{0};             // Total size in bytes
    int64_t downloaded_bytes{0};        // Bytes downloaded
    int64_t uploaded_bytes{0};          // Bytes uploaded (seeding)
    
    int64_t download_speed{0};          // Bytes per second
    int64_t upload_speed{0};            // Bytes per second
    
    std::chrono::seconds eta{0};        // Estimated time remaining
    
    std::optional<std::string> error_message;
    std::optional<std::string> save_path;
    
    // Torrent-specific
    std::optional<float> seed_ratio;
    std::optional<int> num_seeders;
    std::optional<int> num_leechers;
    
    /**
     * @brief Calculate download progress as percentage (0.0 - 100.0)
     */
    [[nodiscard]] double progress() const noexcept {
        if (total_bytes == 0) return 0.0;
        return (static_cast<double>(downloaded_bytes) / static_cast<double>(total_bytes)) * 100.0;
    }
};

/**
 * @brief Global statistics for a download client.
 */
struct GlobalStats {
    int64_t download_speed{0};          // Total download speed (bytes/sec)
    int64_t upload_speed{0};            // Total upload speed (bytes/sec)
    int active_count{0};                // Number of active downloads
    int waiting_count{0};               // Number of queued downloads
    int stopped_count{0};               // Number of stopped/completed
};

/**
 * @brief Abstract interface for download clients.
 * 
 * This interface enables polymorphic handling of different download backends
 * (Aria2, QBittorrent, etc.) through a unified API.
 * 
 * Design rationale:
 * - All methods return std::future for non-blocking operation
 * - Result<T> provides explicit error handling without exceptions
 * - std::string_view for input parameters to avoid copies
 */
class IDownloader {
public:
    virtual ~IDownloader() = default;

    // ========================================================================
    // Task Management
    // ========================================================================
    
    /**
     * @brief Add a download by URI (HTTP/FTP/Magnet).
     * @param uri The URL or magnet link to download
     * @param options Optional key-value options (e.g., {"dir": "/path"})
     * @return Future containing the task ID or error
     */
    virtual std::future<Result<std::string>> addUri(
        std::string_view uri,
        const std::vector<std::pair<std::string, std::string>>& options = {}
    ) = 0;

    /**
     * @brief Add a torrent from file content.
     * @param torrent_data Raw .torrent file bytes
     * @return Future containing the task ID or error
     */
    virtual std::future<Result<std::string>> addTorrent(
        std::string_view torrent_data,
        const std::vector<std::pair<std::string, std::string>>& options = {}
    ) = 0;

    /**
     * @brief Pause an active download.
     * @param id Task identifier
     * @return Future indicating success or error
     */
    virtual std::future<Result<void>> pause(std::string_view id) = 0;

    /**
     * @brief Resume a paused download.
     * @param id Task identifier
     * @return Future indicating success or error
     */
    virtual std::future<Result<void>> resume(std::string_view id) = 0;

    /**
     * @brief Remove a download task.
     * @param id Task identifier
     * @param delete_files If true, delete downloaded files from disk
     * @return Future indicating success or error
     */
    virtual std::future<Result<void>> remove(std::string_view id, bool delete_files = false) = 0;

    // ========================================================================
    // Status Queries
    // ========================================================================
    
    /**
     * @brief Get status of a specific download.
     * @param id Task identifier
     * @return Future containing detailed status or error
     */
    virtual std::future<Result<DownloadStatus>> getStatus(std::string_view id) = 0;

    /**
     * @brief Get all active downloads.
     * @return Future containing list of active download statuses
     */
    virtual std::future<Result<std::vector<DownloadStatus>>> getActiveDownloads() = 0;

    /**
     * @brief Get global client statistics.
     * @return Future containing global stats or error
     */
    virtual std::future<Result<GlobalStats>> getGlobalStats() = 0;

    // ========================================================================
    // Connection Management
    // ========================================================================
    
    /**
     * @brief Check if client is connected and operational.
     * @return true if connected
     */
    [[nodiscard]] virtual bool isConnected() const noexcept = 0;

    /**
     * @brief Get the client type name (e.g., "aria2", "qbittorrent").
     */
    [[nodiscard]] virtual std::string_view clientName() const noexcept = 0;
};

} // namespace cmlb

#endif // CMLB_DOWNLOADERS_IDOWNLOADER_HPP
