#ifndef CMLB_COMMANDS_COMMAND_ROUTER_HPP
#define CMLB_COMMANDS_COMMAND_ROUTER_HPP

#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <memory>

namespace cmlb {

// Forward declarations
class BotEngine;

/**
 * @brief Context passed to command handlers.
 */
struct CommandContext {
    int64_t chat_id;
    int64_t user_id;
    int64_t message_id;
    std::string command;        // Command name without leading /
    std::string args;           // Arguments after the command
    std::string full_text;      // Full message text
    BotEngine* engine;          // Reference to bot engine
};

/**
 * @brief Command handler function signature.
 */
using CommandHandler = std::move_only_function<void(const CommandContext&)>;

/**
 * @brief Routes incoming commands to their handlers.
 * 
 * Supports:
 * - Simple command matching (/mirror, /status)
 * - Command aliases (/m -> /mirror)
 * - Default/fallback handler
 * - Permission checking (admin/user/owner)
 */
class CommandRouter {
public:
    enum class Permission {
        Anyone,     // Any user can use
        User,       // Registered users only
        Admin,      // Admin users only
        Owner       // Bot owner only
    };

    struct CommandInfo {
        std::string name;
        std::string description;
        std::string usage;
        Permission permission{Permission::Anyone};
        CommandHandler handler;
    };

    CommandRouter() = default;

    /**
     * @brief Register a command handler.
     */
    void registerCommand(
        std::string name,
        std::string description,
        CommandHandler handler,
        Permission permission = Permission::Anyone
    );

    /**
     * @brief Register a command alias.
     */
    void registerAlias(std::string alias, std::string target);

    /**
     * @brief Dispatch a command to its handler.
     * @return true if command was handled
     */
    bool dispatch(const CommandContext& ctx) const;

    /**
     * @brief Get all registered commands for help display.
     */
    [[nodiscard]] const std::unordered_map<std::string, CommandInfo>& getCommands() const {
        return commands_;
    }

    /**
     * @brief Check if user has required permission.
     */
    [[nodiscard]] bool checkPermission(const CommandContext& ctx, Permission required) const;

    /**
     * @brief Convert permission to string for logging.
     */
    static std::string permissionToString(Permission perm);

private:
    std::unordered_map<std::string, CommandInfo> commands_;
    std::unordered_map<std::string, std::string> aliases_;
};

// ============================================================================
// Command Handler Declarations
// ============================================================================

// Mirror/Leech commands
void handleMirror(const CommandContext& ctx);
void handleLeech(const CommandContext& ctx);
void handleQbMirror(const CommandContext& ctx);
void handleQbLeech(const CommandContext& ctx);

// Status/Control commands
void handleStatus(const CommandContext& ctx);
void handleCancel(const CommandContext& ctx);
void handleCancelAll(const CommandContext& ctx);
void handlePause(const CommandContext& ctx);
void handleResume(const CommandContext& ctx);

// Settings commands
void handleSettings(const CommandContext& ctx);
void handleBotSettings(const CommandContext& ctx);

// Utility commands
void handleHelp(const CommandContext& ctx);
void handleStats(const CommandContext& ctx);
void handlePing(const CommandContext& ctx);
void handleLog(const CommandContext& ctx);

// Clone/Drive commands
void handleClone(const CommandContext& ctx);
void handleCount(const CommandContext& ctx);
void handleDelete(const CommandContext& ctx);

/**
 * @brief Setup all command handlers on the router.
 */
void setupCommands(CommandRouter& router);

} // namespace cmlb

#endif // CMLB_COMMANDS_COMMAND_ROUTER_HPP
