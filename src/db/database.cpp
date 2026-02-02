#include "db/database.hpp"
#include "core/logger.hpp"

#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cmlb {

// ============================================================================
// JSON Serialization Helpers
// ============================================================================

static void to_json(json& j, const UserSettings& s) {
    j = json{
        {"user_id", s.user_id},
        {"leech_dest_id", s.leech_dest_id ? *s.leech_dest_id : json(nullptr)},
        {"thumbnail_path", s.thumbnail_path ? *s.thumbnail_path : json(nullptr)},
        {"leech_prefix", s.leech_prefix},
        {"split_size", s.split_size},
        {"as_document", s.as_document},
        {"equal_splits", s.equal_splits},
        {"upload_dest", static_cast<int>(s.upload_dest)},
        {"gdrive_folder_id", s.gdrive_folder_id ? *s.gdrive_folder_id : json(nullptr)},
        {"rclone_path", s.rclone_path ? *s.rclone_path : json(nullptr)}
    };
}

static void from_json(const json& j, UserSettings& s) {
    s.user_id = j.value("user_id", 0LL);
    if (!j["leech_dest_id"].is_null()) s.leech_dest_id = j["leech_dest_id"].get<int64_t>();
    if (!j["thumbnail_path"].is_null()) s.thumbnail_path = j["thumbnail_path"].get<std::string>();
    s.leech_prefix = j.value("leech_prefix", "");
    s.split_size = j.value("split_size", 2'000'000'000LL);
    s.as_document = j.value("as_document", false);
    s.equal_splits = j.value("equal_splits", true);
    s.upload_dest = static_cast<UploadDestination>(j.value("upload_dest", 0));
    if (!j["gdrive_folder_id"].is_null()) s.gdrive_folder_id = j["gdrive_folder_id"].get<std::string>();
    if (!j["rclone_path"].is_null()) s.rclone_path = j["rclone_path"].get<std::string>();
}

static void to_json(json& j, const BotSettings& s) {
    j = json{
        {"owner_id", s.owner_id},
        {"sudo_users", s.sudo_users},
        {"authorized_chats", s.authorized_chats},
        {"max_download_size", s.max_download_size},
        {"max_parallel_downloads", s.max_parallel_downloads},
        {"max_parallel_uploads", s.max_parallel_uploads},
        {"daily_limit_per_user", s.daily_limit_per_user},
        {"as_document", s.as_document},
        {"stop_duplicate", s.stop_duplicate},
        {"status_update_interval", s.status_update_interval},
        {"status_limit", s.status_limit},
        {"aria2_url", s.aria2_url},
        {"aria2_secret", s.aria2_secret},
        {"qbit_url", s.qbit_url},
        {"qbit_user", s.qbit_user},
        {"qbit_pass", s.qbit_pass},
        {"rclone_path", s.rclone_path}
    };
}

static void from_json(const json& j, BotSettings& s) {
    s.owner_id = j.value("owner_id", 0LL);
    if (j.contains("sudo_users")) s.sudo_users = j["sudo_users"].get<std::vector<int64_t>>();
    if (j.contains("authorized_chats")) s.authorized_chats = j["authorized_chats"].get<std::vector<int64_t>>();
    s.max_download_size = j.value("max_download_size", 0LL);
    s.max_parallel_downloads = j.value("max_parallel_downloads", 5);
    s.max_parallel_uploads = j.value("max_parallel_uploads", 3);
    s.daily_limit_per_user = j.value("daily_limit_per_user", 0LL);
    s.as_document = j.value("as_document", false);
    s.stop_duplicate = j.value("stop_duplicate", true);
    s.status_update_interval = j.value("status_update_interval", 5);
    s.status_limit = j.value("status_limit", 10);
    s.aria2_url = j.value("aria2_url", "ws://localhost:6800/jsonrpc");
    s.aria2_secret = j.value("aria2_secret", "");
    s.qbit_url = j.value("qbit_url", "");
    s.qbit_user = j.value("qbit_user", "");
    s.qbit_pass = j.value("qbit_pass", "");
    s.rclone_path = j.value("rclone_path", "rclone");
}

static void to_json(json& j, const TaskRecord& t) {
    j = json{
        {"task_id", t.task_id},
        {"user_id", t.user_id},
        {"chat_id", t.chat_id},
        {"message_id", t.message_id},
        {"download_id", t.download_id},
        {"download_type", t.download_type},
        {"url", t.url},
        {"status", t.status},
        {"is_leech", t.is_leech}
    };
    if (t.upload_path) j["upload_path"] = *t.upload_path;
    if (t.error_message) j["error_message"] = *t.error_message;
}

static void from_json(const json& j, TaskRecord& t) {
    t.task_id = j.value("task_id", "");
    t.user_id = j.value("user_id", 0LL);
    t.chat_id = j.value("chat_id", 0LL);
    t.message_id = j.value("message_id", 0LL);
    t.download_id = j.value("download_id", "");
    t.download_type = j.value("download_type", "");
    t.url = j.value("url", "");
    t.status = j.value("status", "");
    t.is_leech = j.value("is_leech", false);
    if (j.contains("upload_path") && !j["upload_path"].is_null()) 
        t.upload_path = j["upload_path"].get<std::string>();
    if (j.contains("error_message") && !j["error_message"].is_null()) 
        t.error_message = j["error_message"].get<std::string>();
}

// ============================================================================
// In-Memory Database Implementation
// ============================================================================

/**
 * @brief In-memory database implementation for development/testing.
 * 
 * Optionally persists data to a JSON file for crash recovery.
 */
class InMemoryDatabase : public IDatabase {
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<int64_t, UserSettings> user_settings_;
    BotSettings bot_settings_;
    std::unordered_map<std::string, TaskRecord> tasks_;
    bool connected_{false};
    std::string persist_path_;

    void loadFromFile() {
        if (persist_path_.empty()) return;
        
        std::ifstream file(persist_path_);
        if (!file.is_open()) return;
        
        try {
            json j;
            file >> j;
            
            if (j.contains("bot_settings")) {
                bot_settings_ = j["bot_settings"].get<BotSettings>();
            }
            
            if (j.contains("user_settings")) {
                for (const auto& [key, value] : j["user_settings"].items()) {
                    user_settings_[std::stoll(key)] = value.get<UserSettings>();
                }
            }
            
            if (j.contains("tasks")) {
                for (const auto& [key, value] : j["tasks"].items()) {
                    tasks_[key] = value.get<TaskRecord>();
                }
            }
            
            Logger::info("Loaded database from {}", persist_path_);
        } catch (const std::exception& e) {
            Logger::warn("Failed to load database: {}", e.what());
        }
    }
    
    void saveToFile() {
        if (persist_path_.empty()) return;
        
        try {
            json j;
            j["bot_settings"] = bot_settings_;
            
            json users = json::object();
            for (const auto& [id, settings] : user_settings_) {
                users[std::to_string(id)] = settings;
            }
            j["user_settings"] = users;
            
            json task_json = json::object();
            for (const auto& [id, task] : tasks_) {
                task_json[id] = task;
            }
            j["tasks"] = task_json;
            
            std::ofstream file(persist_path_);
            file << j.dump(2);
            
        } catch (const std::exception& e) {
            Logger::error("Failed to save database: {}", e.what());
        }
    }

public:
    explicit InMemoryDatabase(const std::string& persist_path = "") 
        : persist_path_(persist_path) 
    {
        if (persist_path.empty()) {
            Logger::info("Using in-memory database (data will not persist)");
        } else {
            Logger::info("Using file-backed database: {}", persist_path);
        }
    }

    ~InMemoryDatabase() {
        if (connected_) {
            saveToFile();
        }
    }

    Result<void> connect() override {
        connected_ = true;
        loadFromFile();
        Logger::info("Database connected");
        return {};
    }

    void disconnect() override {
        if (connected_) {
            saveToFile();
            connected_ = false;
        }
    }

    [[nodiscard]] bool isConnected() const noexcept override {
        return connected_;
    }

    // ========================================================================
    // User Settings
    // ========================================================================

    Result<UserSettings> getUserSettings(int64_t user_id) override {
        std::shared_lock lock(mutex_);
        
        auto it = user_settings_.find(user_id);
        if (it != user_settings_.end()) {
            return it->second;
        }
        
        // Return default settings for new user
        UserSettings defaults;
        defaults.user_id = user_id;
        defaults.created_at = std::chrono::system_clock::now();
        defaults.updated_at = defaults.created_at;
        return defaults;
    }

    Result<void> saveUserSettings(const UserSettings& settings) override {
        std::unique_lock lock(mutex_);
        
        auto modified = settings;
        modified.updated_at = std::chrono::system_clock::now();
        if (user_settings_.find(settings.user_id) == user_settings_.end()) {
            modified.created_at = modified.updated_at;
        }
        
        user_settings_[settings.user_id] = modified;
        saveToFile();
        return {};
    }

    Result<void> deleteUserSettings(int64_t user_id) override {
        std::unique_lock lock(mutex_);
        user_settings_.erase(user_id);
        saveToFile();
        return {};
    }

    // ========================================================================
    // Bot Settings
    // ========================================================================

    Result<BotSettings> getBotSettings() override {
        std::shared_lock lock(mutex_);
        return bot_settings_;
    }

    Result<void> saveBotSettings(const BotSettings& settings) override {
        std::unique_lock lock(mutex_);
        bot_settings_ = settings;
        saveToFile();
        return {};
    }

    // ========================================================================
    // Task Records
    // ========================================================================

    Result<void> saveTask(const TaskRecord& task) override {
        std::unique_lock lock(mutex_);
        
        auto modified = task;
        modified.updated_at = std::chrono::system_clock::now();
        if (tasks_.find(task.task_id) == tasks_.end()) {
            modified.created_at = modified.updated_at;
        }
        
        tasks_[task.task_id] = modified;
        saveToFile();
        return {};
    }

    Result<std::optional<TaskRecord>> getTask(const std::string& task_id) override {
        std::shared_lock lock(mutex_);
        
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    Result<std::vector<TaskRecord>> getIncompleteTasks() override {
        std::shared_lock lock(mutex_);
        
        std::vector<TaskRecord> result;
        for (const auto& [id, task] : tasks_) {
            if (task.status != "complete" && task.status != "error" && task.status != "cancelled") {
                result.push_back(task);
            }
        }
        return result;
    }

    Result<std::vector<TaskRecord>> getUserTasks(int64_t user_id) override {
        std::shared_lock lock(mutex_);
        
        std::vector<TaskRecord> result;
        for (const auto& [id, task] : tasks_) {
            if (task.user_id == user_id) {
                result.push_back(task);
            }
        }
        return result;
    }

    Result<void> deleteTask(const std::string& task_id) override {
        std::unique_lock lock(mutex_);
        tasks_.erase(task_id);
        saveToFile();
        return {};
    }

    Result<void> updateTaskStatus(const std::string& task_id, const std::string& status) override {
        std::unique_lock lock(mutex_);
        
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) {
            it->second.status = status;
            it->second.updated_at = std::chrono::system_clock::now();
            saveToFile();
        }
        return {};
    }
};

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<IDatabase> createMongoDatabase(
    const std::string& connection_uri, 
    const std::string& db_name
) {
    // MongoDB implementation would go here when mongo-cxx-driver is available
    // For now, use file-backed in-memory database
    Logger::warn("MongoDB not available, using file-backed database");
    
    // Use a default path based on db_name
    std::string persist_path = "data/" + db_name + ".json";
    return std::make_unique<InMemoryDatabase>(persist_path);
}

std::unique_ptr<IDatabase> createInMemoryDatabase() {
    return std::make_unique<InMemoryDatabase>();
}

} // namespace cmlb
