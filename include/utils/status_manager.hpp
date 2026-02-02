#ifndef CMLB_UTILS_STATUS_MANAGER_HPP
#define CMLB_UTILS_STATUS_MANAGER_HPP

#include "downloaders/idownloader.hpp"
#include "uploaders/iuploader.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <memory>

namespace cmlb {

/**
 * @brief Unified task status for both downloads and uploads.
 */
struct TaskInfo {
    std::string id;
    std::string name;
    int64_t user_id;
    int64_t chat_id;
    int64_t status_message_id{0};     // Message ID showing status
    
    enum class Type { Download, Upload, Clone, Extract, Archive };
    Type type{Type::Download};
    
    enum class State { Queued, Active, Paused, Complete, Error, Cancelled };
    State state{State::Queued};
    
    int64_t total_bytes{0};
    int64_t processed_bytes{0};
    int64_t speed_bps{0};
    std::chrono::seconds eta{0};
    
    std::string download_client;      // aria2, qbittorrent
    std::string upload_dest;          // telegram, gdrive, rclone
    
    std::optional<std::string> error_message;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point updated_at;
    
    [[nodiscard]] double progress() const noexcept {
        if (total_bytes == 0) return 0.0;
        return (static_cast<double>(processed_bytes) / static_cast<double>(total_bytes)) * 100.0;
    }
};

/**
 * @brief Manages task statuses and message updates.
 * 
 * Features:
 * - Centralized task tracking
 * - Throttled message updates (configurable interval)
 * - Multi-page status with navigation buttons
 * - Progress bar rendering
 */
class StatusManager {
public:
    struct Config {
        int update_interval_sec{5};     // Minimum seconds between message edits
        int tasks_per_page{8};          // Tasks shown per status message
        int progress_bar_width{10};     // Characters in progress bar
    };

    explicit StatusManager(const Config& config = {});
    ~StatusManager();

    /**
     * @brief Register a new task.
     * @return Task ID
     */
    std::string addTask(const TaskInfo& info);

    /**
     * @brief Update task progress from download status.
     */
    void updateFromDownload(const std::string& task_id, const DownloadStatus& status);

    /**
     * @brief Update task progress from upload progress.
     */
    void updateFromUpload(const std::string& task_id, const UploadProgress& progress);

    /**
     * @brief Mark task as complete.
     */
    void completeTask(const std::string& task_id);

    /**
     * @brief Mark task as failed.
     */
    void failTask(const std::string& task_id, const std::string& error);

    /**
     * @brief Remove task from tracking.
     */
    void removeTask(const std::string& task_id);

    /**
     * @brief Get a specific task.
     */
    std::optional<TaskInfo> getTask(const std::string& task_id) const;

    /**
     * @brief Get all active tasks.
     */
    std::vector<TaskInfo> getActiveTasks() const;

    /**
     * @brief Get tasks for a specific user.
     */
    std::vector<TaskInfo> getUserTasks(int64_t user_id) const;

    /**
     * @brief Get paginated active tasks.
     * @param page Zero-indexed page number
     * @return Tasks for the page
     */
    std::vector<TaskInfo> getPage(int page) const;

    /**
     * @brief Get total number of pages.
     */
    int getTotalPages() const;

    /**
     * @brief Format status message for display.
     */
    std::string formatStatusMessage(int page = 0) const;

    /**
     * @brief Check if message for task needs update (throttling).
     */
    bool needsUpdate(const std::string& task_id) const;

    /**
     * @brief Mark task message as updated.
     */
    void markUpdated(const std::string& task_id);

    // ========================================================================
    // Rendering Utilities
    // ========================================================================
    
    /**
     * @brief Render a progress bar.
     */
    static std::string renderProgressBar(double percent, int width = 10);

    /**
     * @brief Format bytes as human-readable string.
     */
    static std::string formatBytes(int64_t bytes);

    /**
     * @brief Format duration as human-readable string.
     */
    static std::string formatDuration(std::chrono::seconds duration);

    /**
     * @brief Format speed as human-readable string.
     */
    static std::string formatSpeed(int64_t bytes_per_sec);

private:
    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TaskInfo> tasks_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_update_;
    
    std::string generateTaskId() const;
};

} // namespace cmlb

#endif // CMLB_UTILS_STATUS_MANAGER_HPP
