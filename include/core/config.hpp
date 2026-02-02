#ifndef CMLB_CORE_CONFIG_HPP
#define CMLB_CORE_CONFIG_HPP

#include "core/types.hpp"
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace cmlb {

struct Aria2Config {
    std::string rpc_url = "ws://localhost:6800/jsonrpc";
    std::string secret = "";
    int max_concurrent_downloads = 5;
};

struct QBittorrentConfig {
    std::string url = "";
    std::string username = "";
    std::string password = "";
    double seed_ratio_limit = 0.0;
    int seed_time_limit = 0;
};

struct TelegramConfig {
    int32_t api_id = 0;
    std::string api_hash;
    std::string database_directory = "tdlib";
    int64_t owner_id = 0;                       // Bot owner user ID
    std::vector<int64_t> sudo_users;            // Admin user IDs
    std::vector<int64_t> authorized_chats;      // Allowed chat IDs
};

struct RcloneConfig {
    std::string path = "rclone";
    std::string config_path;
};

struct AppConfig {
    TelegramConfig telegram;
    Aria2Config aria2;
    QBittorrentConfig qbittorrent;
    RcloneConfig rclone;
    std::string download_dir = "downloads";
    std::string log_level = "info";

    static Result<AppConfig> load(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(AppError(ErrorCode::InternalError, "Config file not found: " + path));
        }

        try {
            std::ifstream f(path);
            nlohmann::json j = nlohmann::json::parse(f);

            AppConfig cfg;
            
            // Telegram settings
            if (j.contains("telegram")) {
                auto& t = j["telegram"];
                cfg.telegram.api_id = t.value("api_id", 0);
                cfg.telegram.api_hash = t.value("api_hash", "");
                cfg.telegram.database_directory = t.value("database_directory", "tdlib");
                cfg.telegram.owner_id = t.value("owner_id", 0LL);
                if (t.contains("sudo_users")) {
                    cfg.telegram.sudo_users = t["sudo_users"].get<std::vector<int64_t>>();
                }
                if (t.contains("authorized_chats")) {
                    cfg.telegram.authorized_chats = t["authorized_chats"].get<std::vector<int64_t>>();
                }
            }

            // Aria2 settings
            if (j.contains("aria2")) {
                auto& a = j["aria2"];
                cfg.aria2.rpc_url = a.value("rpc_url", "ws://localhost:6800/jsonrpc");
                cfg.aria2.secret = a.value("secret", "");
                cfg.aria2.max_concurrent_downloads = a.value("max_concurrent_downloads", 5);
            }

            // QBittorrent settings
            if (j.contains("qbittorrent")) {
                auto& q = j["qbittorrent"];
                cfg.qbittorrent.url = q.value("url", "");
                cfg.qbittorrent.username = q.value("username", "");
                cfg.qbittorrent.password = q.value("password", "");
                cfg.qbittorrent.seed_ratio_limit = q.value("seed_ratio_limit", 0.0);
                cfg.qbittorrent.seed_time_limit = q.value("seed_time_limit", 0);
            }

            // Rclone settings
            if (j.contains("rclone")) {
                auto& r = j["rclone"];
                cfg.rclone.path = r.value("path", "rclone");
                cfg.rclone.config_path = r.value("config_path", "");
            }

            // General settings
            cfg.download_dir = j.value("download_dir", "downloads");
            cfg.log_level = j.value("log_level", "info");

            // Environment variable overrides
            if (const char* env = std::getenv("TELEGRAM_API_ID")) {
                cfg.telegram.api_id = std::stoi(env);
            }
            if (const char* env = std::getenv("TELEGRAM_API_HASH")) {
                cfg.telegram.api_hash = env;
            }
            if (const char* env = std::getenv("OWNER_ID")) {
                cfg.telegram.owner_id = std::stoll(env);
            }
            if (const char* env = std::getenv("ARIA2_RPC_URL")) {
                cfg.aria2.rpc_url = env;
            }
            if (const char* env = std::getenv("ARIA2_SECRET")) {
                cfg.aria2.secret = env;
            }

            if (cfg.telegram.api_id == 0 || cfg.telegram.api_hash.empty()) {
                return std::unexpected(AppError(ErrorCode::InternalError, "Missing required Telegram credentials in config"));
            }

            return cfg;

        } catch (const std::exception& e) {
            return std::unexpected(AppError(ErrorCode::JsonParseError, e.what()));
        }
    }
};

} // namespace cmlb

#endif // CMLB_CORE_CONFIG_HPP
