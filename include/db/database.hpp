#ifndef CMLB_DB_DATABASE_HPP
#define CMLB_DB_DATABASE_HPP

#include "core/types.hpp"
#include <string>
#include <optional>
#include <chrono>
#include <vector>
#include <memory>
#include <functional>

namespace cmlb {

// Forward declare for BotEngine access
enum class UploadDestination {
    Telegram,
    GoogleDrive,
    Rclone
};

// ============================================================================
// Database Schemas
// ============================================================================

/**
 * @brief User-specific settings stored in database.
 */
struct UserSettings {
    int64_t user_id{0};
    
    // Upload preferences
    std::optional<int64_t> leech_dest_id;           // Custom upload destination chat
    std::optional<std::string> thumbnail_path;      // Custom thumbnail file path
    std::string leech_prefix;                       // Filename prefix
    int64_t split_size{2'000'000'000};             // 2GB default
    bool as_document{false};                        // Upload as doc vs media
    bool equal_splits{true};                        // Equal size splits
    
    // Download preferences
    UploadDestination upload_dest{UploadDestination::Telegram};
    std::optional<std::string> gdrive_folder_id;
    std::optional<std::string> rclone_path;
    
    // Timestamps
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

/**
 * @brief Bot-wide settings stored in database.
 */
struct BotSettings {
    int64_t owner_id{0};                            // Bot owner user ID
    std::vector<int64_t> sudo_users;                // Admin user IDs
    std::vector<int64_t> authorized_chats;          // Allowed chat IDs
    
    // Global limits
    int64_t max_download_size{0};                   // 0 = unlimited
    int max_parallel_downloads{5};
    int max_parallel_uploads{3};
    int64_t daily_limit_per_user{0};               // 0 = unlimited
    
    // Default behaviors
    bool as_document{false};
    bool stop_duplicate{true};
    int status_update_interval{5};                  // Seconds
    int status_limit{10};                           // Tasks per page
    
    // Aria2 settings
    std::string aria2_url{"ws://localhost:6800/jsonrpc"};
    std::string aria2_secret;
    std::optional<std::string> aria2_max_download_limit;
    std::optional<std::string> aria2_max_upload_limit;
    
    // QBittorrent settings
    std::string qbit_url;
    std::string qbit_user;
    std::string qbit_pass;
    std::optional<double> seed_ratio_limit;
    std::optional<int> seed_time_limit;             // Minutes
    
    // Rclone settings
    std::string rclone_path{"rclone"};
    std::optional<std::string> rclone_config_path;
};

/**
 * @brief Represents an incomplete/active task for recovery.
 */
struct TaskRecord {
    std::string task_id;
    int64_t user_id;
    int64_t chat_id;
    int64_t message_id;
    
    std::string download_id;                        // GID or hash
    std::string download_type;                      // aria2, qbittorrent
    std::string url;
    std::string status;                             // downloading, uploading, etc.
    bool is_leech{false};                           // Upload to Telegram vs cloud
    
    std::optional<std::string> upload_path;
    std::optional<std::string> error_message;
    
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

// ============================================================================
// Database Interface
// ============================================================================

/**
 * @brief Abstract database interface for persistence operations.
 * 
 * This allows swapping MongoDB for SQLite or other backends if needed.
 */
class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Connection
    virtual Result<void> connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool isConnected() const noexcept = 0;

    // User Settings
    virtual Result<UserSettings> getUserSettings(int64_t user_id) = 0;
    virtual Result<void> saveUserSettings(const UserSettings& settings) = 0;
    virtual Result<void> deleteUserSettings(int64_t user_id) = 0;

    // Bot Settings (singleton)
    virtual Result<BotSettings> getBotSettings() = 0;
    virtual Result<void> saveBotSettings(const BotSettings& settings) = 0;

    // Task Records
    virtual Result<void> saveTask(const TaskRecord& task) = 0;
    virtual Result<std::optional<TaskRecord>> getTask(const std::string& task_id) = 0;
    virtual Result<std::vector<TaskRecord>> getIncompleteTasks() = 0;
    virtual Result<std::vector<TaskRecord>> getUserTasks(int64_t user_id) = 0;
    virtual Result<void> deleteTask(const std::string& task_id) = 0;
    virtual Result<void> updateTaskStatus(const std::string& task_id, const std::string& status) = 0;
};

/**
 * @brief Create a MongoDB database instance.
 * @param connection_uri MongoDB connection string
 * @param db_name Database name
 */
std::unique_ptr<IDatabase> createMongoDatabase(
    const std::string& connection_uri, 
    const std::string& db_name
);

/**
 * @brief Create an in-memory database instance (for testing/development).
 */
std::unique_ptr<IDatabase> createInMemoryDatabase();

} // namespace cmlb

#endif // CMLB_DB_DATABASE_HPP
