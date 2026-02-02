#ifndef CMLB_CORE_BOT_ENGINE_HPP
#define CMLB_CORE_BOT_ENGINE_HPP

#include "core/types.hpp"
#include "core/config.hpp"
#include "downloaders/idownloader.hpp"
#include "db/database.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace cmlb {

// Forward declarations
class Aria2Downloader;
class CommandRouter;
class IUploader;
class StatusManager;

/**
 * @brief Destination for uploads.
 */
enum class UploadDestination {
    Telegram,
    GoogleDrive,
    Rclone
};

/**
 * @brief User settings structure matching database schema.
 */
struct UserSettingsView {
    int64_t user_id{0};
    std::optional<int64_t> leech_dest_id;
    std::optional<std::string> thumbnail_path;
    bool as_document{false};
    int64_t split_size{2'000'000'000};
    UploadDestination upload_dest{UploadDestination::Telegram};
    std::optional<std::string> gdrive_folder_id;
    std::optional<std::string> rclone_path;
};

/**
 * @brief Bot-wide settings view.
 */
struct BotSettingsView {
    int64_t owner_id{0};
    std::vector<int64_t> sudo_users;
    std::vector<int64_t> authorized_chats;
    int max_parallel_downloads{5};
    int64_t max_download_size{0};  // 0 = unlimited
    int64_t daily_limit_per_user{0};  // 0 = unlimited
    std::string aria2_url;
    std::string aria2_secret;
};

/**
 * @brief The main engine of the bot.
 */
class BotEngine {
public:
    explicit BotEngine(const AppConfig& config);
    ~BotEngine();

    // Disable copy/move
    BotEngine(const BotEngine&) = delete;
    BotEngine& operator=(const BotEngine&) = delete;

    /**
     * @brief Starts the main event loop.
     * Blocks until requestStop() is called or signal received.
     */
    void run();

    /**
     * @brief Thread-safe request to stop the engine.
     * Can be called from any thread, including signal handlers.
     */
    void requestStop() noexcept;

    /**
     * @brief Enqueues a task to be executed on the main thread.
     */
    void enqueueTask(std::move_only_function<void()> task);

    /**
     * @brief Get the active downloader client.
     */
    [[nodiscard]] IDownloader* getDownloader() const noexcept { return downloader_.get(); }

    /**
     * @brief Get the QBittorrent downloader (if configured).
     */
    [[nodiscard]] IDownloader* getQBittorrentDownloader() const noexcept { return qbit_downloader_.get(); }

    /**
     * @brief Get the database interface.
     */
    [[nodiscard]] IDatabase* getDatabase() const noexcept { return database_.get(); }

    // ========================================================================
    // Public API for command handlers
    // ========================================================================
    
    /**
     * @brief Send a text message to a chat.
     */
    void sendMessage(int64_t chat_id, const std::string& text);

    /**
     * @brief Send a message with inline keyboard for settings.
     */
    void sendSettingsMenu(int64_t chat_id, int64_t user_id, const std::string& text);

    /**
     * @brief Send a file to a chat.
     */
    void sendFile(int64_t chat_id, const std::string& file_path, const std::string& caption = "");

    /**
     * @brief Send the log file to a chat.
     */
    void sendLogFile(int64_t chat_id);

    /**
     * @brief Start a mirror/leech operation.
     * @param leech If true, upload to Telegram; if false, upload to cloud.
     */
    void startMirror(int64_t chat_id, int64_t user_id, const std::string& url, bool leech);

    /**
     * @brief Start a QBittorrent mirror/leech operation.
     */
    void startQbMirror(int64_t chat_id, int64_t user_id, const std::string& url, bool leech);

    /**
     * @brief Clone a Google Drive file/folder.
     */
    void startClone(int64_t chat_id, int64_t user_id, const std::string& url);

    /**
     * @brief Count files in a Google Drive folder.
     */
    void countDriveFiles(int64_t chat_id, const std::string& url);

    /**
     * @brief Delete a file from Google Drive.
     */
    void deleteDriveFile(int64_t chat_id, const std::string& url);

    /**
     * @brief Get the bot uptime as a formatted string.
     */
    [[nodiscard]] std::string getUptimeString() const;

private:
    void processResponse(td::ClientManager::Response response);
    void handleUpdate(std::unique_ptr<td::td_api::Object> update);
    void onNewMessage(const td::td_api::updateNewMessage& update);
    
    // Legacy command handlers (kept for backward compatibility)
    void handleMirrorCommand(int64_t chatId, const std::string& text);
    void handleStatusCommand(int64_t chatId);
    void handleCancelCommand(int64_t chatId, const std::string& text);
    
    void sendTextMessage(int64_t chatId, const std::string& text);

    // Task management
    void trackDownload(int64_t chat_id, int64_t user_id, const std::string& gid, bool leech);
    void onDownloadComplete(const std::string& gid, int64_t chat_id, int64_t user_id, bool leech);

private:
    std::unique_ptr<td::ClientManager> td_client_manager_;
    std::int32_t td_client_id_{0};
    
    AppConfig config_;
    std::unique_ptr<IDownloader> downloader_;
    std::unique_ptr<IDownloader> qbit_downloader_;
    std::unique_ptr<IDatabase> database_;
    std::unique_ptr<CommandRouter> command_router_;
    std::unique_ptr<IUploader> telegram_uploader_;
    std::unique_ptr<IUploader> rclone_uploader_;
    std::unique_ptr<StatusManager> status_manager_;

    // Thread-safe job queue
    std::queue<std::move_only_function<void()>> job_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    std::atomic<bool> stop_flag_{false};
    std::chrono::steady_clock::time_point start_time_;
    
    std::vector<std::jthread> background_tasks_;
    std::mutex tasks_mutex_;
};

} // namespace cmlb

#endif // CMLB_CORE_BOT_ENGINE_HPP
