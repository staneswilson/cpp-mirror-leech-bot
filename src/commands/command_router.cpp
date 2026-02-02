#include "commands/command_router.hpp"
#include "core/bot_engine.hpp"
#include "core/logger.hpp"
#include "downloaders/idownloader.hpp"
#include "db/database.hpp"

#include <format>
#include <chrono>
#include <sstream>
#include <array>

namespace cmlb {

// ============================================================================
// CommandRouter Implementation
// ============================================================================

void CommandRouter::registerCommand(
    std::string name,
    std::string description,
    CommandHandler handler,
    Permission permission) 
{
    CommandInfo info;
    info.name = name;
    info.description = description;
    info.permission = permission;
    info.handler = std::move(handler);
    
    commands_[name] = std::move(info);
}

void CommandRouter::registerAlias(std::string alias, std::string target) {
    aliases_[alias] = target;
}

bool CommandRouter::dispatch(const CommandContext& ctx) const {
    std::string cmd = ctx.command;
    
    // Check aliases
    auto alias_it = aliases_.find(cmd);
    if (alias_it != aliases_.end()) {
        cmd = alias_it->second;
    }
    
    // Find handler
    auto it = commands_.find(cmd);
    if (it == commands_.end()) {
        return false;
    }
    
    // Check permissions
    const auto& cmd_info = it->second;
    if (!checkPermission(ctx, cmd_info.permission)) {
        Logger::warn("User {} denied access to command '{}' (requires {})", 
                     ctx.user_id, cmd, permissionToString(cmd_info.permission));
        return true; // Command was recognized but user lacks permission
    }
    
    // Execute handler
    try {
        if (cmd_info.handler) {
            cmd_info.handler(ctx);
        }
    } catch (const std::exception& e) {
        Logger::error("Command '{}' threw exception: {}", cmd, e.what());
    }
    
    return true;
}

bool CommandRouter::checkPermission(const CommandContext& ctx, Permission required) const {
    switch (required) {
        case Permission::Anyone:
            return true;
            
        case Permission::User: {
            // Check if user is registered (has settings in database)
            auto* db = ctx.engine->getDatabase();
            if (!db) return false;
            auto settings = db->getUserSettings(ctx.user_id);
            return settings.has_value();
        }
        
        case Permission::Admin: {
            // Check if user is in admin list from bot settings
            auto* db = ctx.engine->getDatabase();
            if (!db) return false;
            auto bot_settings = db->getBotSettings();
            if (!bot_settings) return false;
            
            const auto& admins = bot_settings->sudo_users;
            return std::find(admins.begin(), admins.end(), ctx.user_id) != admins.end();
        }
        
        case Permission::Owner: {
            auto* db = ctx.engine->getDatabase();
            if (!db) return false;
            auto bot_settings = db->getBotSettings();
            if (!bot_settings) return false;
            return ctx.user_id == bot_settings->owner_id;
        }
    }
    return false;
}

std::string CommandRouter::permissionToString(Permission perm) {
    switch (perm) {
        case Permission::Anyone: return "Anyone";
        case Permission::User: return "User";
        case Permission::Admin: return "Admin";
        case Permission::Owner: return "Owner";
    }
    return "Unknown";
}

// ============================================================================
// Utility Functions
// ============================================================================

static std::string formatBytes(int64_t bytes) {
    constexpr std::array<const char*, 5> units = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }
    
    return std::format("{:.2f} {}", size, units[unit_idx]);
}

static std::string formatEta(std::chrono::seconds eta) {
    if (eta.count() <= 0) return "N/A";
    
    auto hours = std::chrono::duration_cast<std::chrono::hours>(eta);
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(eta - hours);
    auto secs = eta - hours - mins;
    
    if (hours.count() > 0) {
        return std::format("{}h {}m", hours.count(), mins.count());
    } else if (mins.count() > 0) {
        return std::format("{}m {}s", mins.count(), secs.count());
    } else {
        return std::format("{}s", secs.count());
    }
}

static std::string renderProgressBar(double percent, int width = 10) {
    int filled = static_cast<int>(percent / 100.0 * width);
    int empty = width - filled;
    
    std::string bar;
    for (int i = 0; i < filled; i++) bar += "█";
    for (int i = 0; i < empty; i++) bar += "░";
    
    return std::format("[{}] {:.1f}%", bar, percent);
}

// ============================================================================
// Command Handlers
// ============================================================================

void handleMirror(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a URL to mirror.\n\nUsage: /mirror <url>");
        return;
    }
    
    Logger::info("Mirror: {} from user {}", ctx.args, ctx.user_id);
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader || !downloader->isConnected()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available. Please try again later.");
        return;
    }
    
    // Start the download
    ctx.engine->startMirror(ctx.chat_id, ctx.user_id, ctx.args, false);
}

void handleLeech(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a URL to leech.\n\nUsage: /leech <url>");
        return;
    }
    
    Logger::info("Leech: {} from user {}", ctx.args, ctx.user_id);
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader || !downloader->isConnected()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available. Please try again later.");
        return;
    }
    
    // Start the download with leech mode (upload to Telegram)
    ctx.engine->startMirror(ctx.chat_id, ctx.user_id, ctx.args, true);
}

void handleQbMirror(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a torrent/magnet.\n\nUsage: /qbmirror <torrent/magnet>");
        return;
    }
    
    Logger::info("QBittorrent mirror: {} from user {}", ctx.args, ctx.user_id);
    
    // Check for QBittorrent downloader
    auto* downloader = ctx.engine->getQBittorrentDownloader();
    if (!downloader || !downloader->isConnected()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ QBittorrent not available. Use /mirror instead.");
        return;
    }
    
    ctx.engine->startQbMirror(ctx.chat_id, ctx.user_id, ctx.args, false);
}

void handleQbLeech(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a torrent/magnet.\n\nUsage: /qbleech <torrent/magnet>");
        return;
    }
    
    Logger::info("QBittorrent leech: {} from user {}", ctx.args, ctx.user_id);
    
    auto* downloader = ctx.engine->getQBittorrentDownloader();
    if (!downloader || !downloader->isConnected()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ QBittorrent not available. Use /leech instead.");
        return;
    }
    
    ctx.engine->startQbMirror(ctx.chat_id, ctx.user_id, ctx.args, true);
}

void handleStatus(const CommandContext& ctx) {
    Logger::info("Status request from user {}", ctx.user_id);
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available.");
        return;
    }
    
    // Get active downloads
    auto future = downloader->getActiveDownloads();
    auto result = future.get();
    
    if (!result) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Failed to get status: " + result.error().message);
        return;
    }
    
    if (result->empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "📭 No active downloads.");
        return;
    }
    
    std::ostringstream oss;
    oss << "📥 **Active Downloads** (" << result->size() << ")\n\n";
    
    int count = 0;
    for (const auto& dl : *result) {
        if (++count > 10) {
            oss << "\n... and " << (result->size() - 10) << " more";
            break;
        }
        
        std::string name = dl.name.length() > 25 ? dl.name.substr(0, 22) + "..." : dl.name;
        oss << "• **" << name << "**\n";
        oss << "  " << renderProgressBar(dl.progress()) << "\n";
        oss << "  ⬇️ " << formatBytes(dl.download_speed) << "/s";
        if (dl.upload_speed > 0) {
            oss << " | ⬆️ " << formatBytes(dl.upload_speed) << "/s";
        }
        oss << " | ETA: " << formatEta(dl.eta) << "\n";
        oss << "  " << formatBytes(dl.downloaded_bytes) << " / " << formatBytes(dl.total_bytes) << "\n\n";
    }
    
    ctx.engine->sendMessage(ctx.chat_id, oss.str());
}

void handleCancel(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide GID to cancel.\n\nUsage: /cancel <gid>");
        return;
    }
    
    Logger::info("Cancel: {} from user {}", ctx.args, ctx.user_id);
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available.");
        return;
    }
    
    std::string gid = ctx.args;
    // Trim whitespace
    gid.erase(0, gid.find_first_not_of(" \t\n\r"));
    gid.erase(gid.find_last_not_of(" \t\n\r") + 1);
    
    auto future = downloader->remove(gid, true);
    auto result = future.get();
    
    if (result) {
        ctx.engine->sendMessage(ctx.chat_id, "✅ Download cancelled: " + gid);
        Logger::info("Cancelled: {}", gid);
    } else {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Failed to cancel: " + result.error().message);
        Logger::error("Cancel failed: {}", result.error().message);
    }
}

void handleCancelAll(const CommandContext& ctx) {
    Logger::info("Cancel all from user {}", ctx.user_id);
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available.");
        return;
    }
    
    auto future = downloader->getActiveDownloads();
    auto result = future.get();
    
    if (!result || result->empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "📭 No active downloads to cancel.");
        return;
    }
    
    int cancelled = 0;
    int failed = 0;
    
    for (const auto& dl : *result) {
        auto remove_result = downloader->remove(dl.id, true).get();
        if (remove_result) {
            cancelled++;
        } else {
            failed++;
        }
    }
    
    std::string msg = std::format("✅ Cancelled {} downloads", cancelled);
    if (failed > 0) {
        msg += std::format(" ({} failed)", failed);
    }
    ctx.engine->sendMessage(ctx.chat_id, msg);
    Logger::info("Cancelled {} downloads, {} failed", cancelled, failed);
}

void handlePause(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Usage: /pause <gid>");
        return;
    }
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available.");
        return;
    }
    
    auto result = downloader->pause(ctx.args).get();
    if (result) {
        ctx.engine->sendMessage(ctx.chat_id, "⏸️ Paused: " + ctx.args);
        Logger::info("Paused: {}", ctx.args);
    } else {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Failed to pause: " + result.error().message);
    }
}

void handleResume(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Usage: /resume <gid>");
        return;
    }
    
    auto* downloader = ctx.engine->getDownloader();
    if (!downloader) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Download client not available.");
        return;
    }
    
    auto result = downloader->resume(ctx.args).get();
    if (result) {
        ctx.engine->sendMessage(ctx.chat_id, "▶️ Resumed: " + ctx.args);
        Logger::info("Resumed: {}", ctx.args);
    } else {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Failed to resume: " + result.error().message);
    }
}

void handleSettings(const CommandContext& ctx) {
    Logger::info("Settings request from user {}", ctx.user_id);
    
    auto* db = ctx.engine->getDatabase();
    if (!db) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Database not available.");
        return;
    }
    
    auto settings = db->getUserSettings(ctx.user_id);
    
    std::ostringstream oss;
    oss << "⚙️ **Your Settings**\n\n";
    
    if (settings) {
        oss << "📤 **Leech Destination**: " << (settings->leech_dest_id ? std::to_string(*settings->leech_dest_id) : "Current Chat") << "\n";
        oss << "🖼️ **Thumbnail**: " << (settings->thumbnail_path ? "Set" : "Not set") << "\n";
        oss << "📂 **As Document**: " << (settings->as_document ? "Yes" : "No") << "\n";
        oss << "✂️ **Split Size**: " << formatBytes(settings->split_size) << "\n";
        oss << "☁️ **Upload Dest**: ";
        switch (settings->upload_dest) {
            case UploadDestination::Telegram: oss << "Telegram"; break;
            case UploadDestination::GoogleDrive: oss << "Google Drive"; break;
            case UploadDestination::Rclone: oss << "Rclone"; break;
        }
        oss << "\n";
    } else {
        oss << "No custom settings. Using defaults.\n";
    }
    
    oss << "\nUse inline buttons below to change settings.";
    
    // Send with inline keyboard
    ctx.engine->sendSettingsMenu(ctx.chat_id, ctx.user_id, oss.str());
}

void handleBotSettings(const CommandContext& ctx) {
    Logger::info("Bot settings request from user {}", ctx.user_id);
    
    auto* db = ctx.engine->getDatabase();
    if (!db) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Database not available.");
        return;
    }
    
    auto settings = db->getBotSettings();
    if (!settings) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Could not load bot settings.");
        return;
    }
    
    std::ostringstream oss;
    oss << "🤖 **Bot Settings**\n\n";
    oss << "📊 **Limits**\n";
    oss << "  • Max parallel downloads: " << settings->max_parallel_downloads << "\n";
    oss << "  • Max download size: " << formatBytes(settings->max_download_size) << "\n";
    oss << "  • Daily user limit: " << formatBytes(settings->daily_limit_per_user) << "\n\n";
    oss << "👥 **Sudo Users**: " << settings->sudo_users.size() << "\n";
    oss << "🚫 **Authorized Chats**: " << settings->authorized_chats.size() << "\n\n";
    oss << "⬇️ **Aria2**\n";
    oss << "  • URL: " << settings->aria2_url << "\n";
    oss << "  • Connected: " << (ctx.engine->getDownloader() && ctx.engine->getDownloader()->isConnected() ? "Yes" : "No") << "\n";
    
    ctx.engine->sendMessage(ctx.chat_id, oss.str());
}

void handleHelp(const CommandContext& ctx) {
    Logger::info("Help request from user {}", ctx.user_id);
    
    std::ostringstream oss;
    oss << "📚 **CMLB Bot Commands**\n\n";
    
    oss << "**Mirror/Leech**\n";
    oss << "/mirror (m) - Mirror URL to cloud\n";
    oss << "/leech (l) - Download and send to Telegram\n";
    oss << "/qbmirror (qm) - Mirror via QBittorrent\n";
    oss << "/qbleech (ql) - Leech via QBittorrent\n\n";
    
    oss << "**Control**\n";
    oss << "/status - Show active downloads\n";
    oss << "/cancel <gid> - Cancel a download\n";
    oss << "/pause <gid> - Pause a download\n";
    oss << "/resume <gid> - Resume a download\n\n";
    
    oss << "**Settings**\n";
    oss << "/settings - User settings\n\n";
    
    oss << "**Utility**\n";
    oss << "/stats - System statistics\n";
    oss << "/ping - Check bot latency\n";
    
    ctx.engine->sendMessage(ctx.chat_id, oss.str());
}

void handleStats(const CommandContext& ctx) {
    Logger::info("Stats request from user {}", ctx.user_id);
    
    std::ostringstream oss;
    oss << "📊 **System Statistics**\n\n";
    
    // Get downloader stats
    auto* downloader = ctx.engine->getDownloader();
    if (downloader && downloader->isConnected()) {
        auto stats_future = downloader->getGlobalStats();
        auto stats_result = stats_future.get();
        
        if (stats_result) {
            oss << "**Downloads**\n";
            oss << "  ⬇️ Speed: " << formatBytes(stats_result->download_speed) << "/s\n";
            oss << "  ⬆️ Speed: " << formatBytes(stats_result->upload_speed) << "/s\n";
            oss << "  📥 Active: " << stats_result->active_count << "\n";
            oss << "  ⏳ Waiting: " << stats_result->waiting_count << "\n";
            oss << "  ⏹️ Stopped: " << stats_result->stopped_count << "\n\n";
        }
    }
    
    // System info (simplified - would use platform APIs in production)
    oss << "**System**\n";
    oss << "  🖥️ Bot: CMLB v0.2.0\n";
    oss << "  ⏰ Uptime: " << ctx.engine->getUptimeString() << "\n";
    
    ctx.engine->sendMessage(ctx.chat_id, oss.str());
}

void handlePing(const CommandContext& ctx) {
    auto start = std::chrono::steady_clock::now();
    
    // Simple ping response
    auto end = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    ctx.engine->sendMessage(ctx.chat_id, std::format("🏓 Pong! Latency: {}ms", latency.count()));
    Logger::info("Ping from user {}", ctx.user_id);
}

void handleLog(const CommandContext& ctx) {
    Logger::info("Log request from user {}", ctx.user_id);
    
    // Send the log file
    ctx.engine->sendLogFile(ctx.chat_id);
}

void handleClone(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a Google Drive link.\n\nUsage: /clone <gdrive_link>");
        return;
    }
    
    Logger::info("Clone: {} from user {}", ctx.args, ctx.user_id);
    ctx.engine->startClone(ctx.chat_id, ctx.user_id, ctx.args);
}

void handleCount(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a Google Drive folder link.\n\nUsage: /count <gdrive_folder_link>");
        return;
    }
    
    Logger::info("Count: {} from user {}", ctx.args, ctx.user_id);
    ctx.engine->countDriveFiles(ctx.chat_id, ctx.args);
}

void handleDelete(const CommandContext& ctx) {
    if (ctx.args.empty()) {
        ctx.engine->sendMessage(ctx.chat_id, "❌ Please provide a Google Drive file/folder link.\n\nUsage: /del <gdrive_link>");
        return;
    }
    
    Logger::info("Delete: {} from user {}", ctx.args, ctx.user_id);
    ctx.engine->deleteDriveFile(ctx.chat_id, ctx.args);
}

// ============================================================================
// Setup Function
// ============================================================================

void setupCommands(CommandRouter& router) {
    // Mirror/Leech
    router.registerCommand("mirror", "Mirror URL to cloud", handleMirror);
    router.registerCommand("leech", "Download and send to Telegram", handleLeech);
    router.registerCommand("qbmirror", "Mirror torrent via QBittorrent", handleQbMirror);
    router.registerCommand("qbleech", "Leech torrent via QBittorrent", handleQbLeech);
    
    // Aliases
    router.registerAlias("m", "mirror");
    router.registerAlias("l", "leech");
    router.registerAlias("qm", "qbmirror");
    router.registerAlias("ql", "qbleech");
    
    // Control
    router.registerCommand("status", "Show active downloads", handleStatus);
    router.registerCommand("cancel", "Cancel a download", handleCancel);
    router.registerCommand("cancelall", "Cancel all downloads", handleCancelAll, CommandRouter::Permission::Admin);
    router.registerCommand("pause", "Pause a download", handlePause);
    router.registerCommand("resume", "Resume a download", handleResume);
    
    // Settings
    router.registerCommand("settings", "User settings", handleSettings);
    router.registerCommand("bsetting", "Bot settings", handleBotSettings, CommandRouter::Permission::Admin);
    
    // Utility
    router.registerCommand("help", "Show help", handleHelp);
    router.registerCommand("start", "Show help", handleHelp);
    router.registerCommand("stats", "System statistics", handleStats);
    router.registerCommand("ping", "Check bot latency", handlePing);
    router.registerCommand("log", "Get log file", handleLog, CommandRouter::Permission::Owner);
    
    // Drive operations
    router.registerCommand("clone", "Clone Google Drive", handleClone);
    router.registerCommand("count", "Count Drive files", handleCount);
    router.registerCommand("del", "Delete from Drive", handleDelete, CommandRouter::Permission::Admin);
    
    Logger::info("Registered {} commands", router.getCommands().size());
}

} // namespace cmlb
