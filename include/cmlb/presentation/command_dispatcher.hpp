#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/presentation/command_parser.hpp>

/// @file command_dispatcher.hpp
/// @brief Maps parsed `CommandRequest` values to authorised use case calls.
///
/// `CommandDispatcher` is the single fan-out point between the Telegram
/// adapter (which produces `CommandRequest` values) and the application layer
/// (which exposes use cases). Each command has a registered required
/// permission level — the dispatcher consults the injected `Authority` before
/// invoking the handler.

// ---------------------------------------------------------------------------
// Forward declarations of cross-layer collaborators.
//
// `CommandDispatcher` references these only by reference, so the dispatcher
// header does not need their full definitions. The `.cpp` translation unit
// includes the concrete headers.
// ---------------------------------------------------------------------------
namespace cmlb::application {
class MirrorUrl;
class LeechUrl;
class CloneDriveResource;
class CountDriveResource;
class DeleteDriveResource;
class CancelTask;
class PauseTask;
class ResumeTask;
class UpdateUserSettings;
class UpdateBotSettings;
class RssSubscription;
} // namespace cmlb::application

namespace cmlb::infrastructure::telegram {
class MessengerInterface;
} // namespace cmlb::infrastructure::telegram

namespace cmlb::infrastructure::system {
class SystemMetrics;
} // namespace cmlb::infrastructure::system

namespace cmlb::presentation {

/// Routes parsed commands to use cases after permission checking.
class CommandDispatcher {
public:
    /// Bundle of every external collaborator the dispatcher needs.
    ///
    /// All references are non-owning — callers must guarantee that every
    /// referenced object outlives the dispatcher (typically: bot process
    /// lifetime).
    struct Dependencies {
        cmlb::domain::Authority authority;
        cmlb::application::MirrorUrl& mirror_url;
        cmlb::application::LeechUrl& leech_url;
        cmlb::application::CloneDriveResource& clone;
        cmlb::application::CountDriveResource& count;
        cmlb::application::DeleteDriveResource& delete_resource;
        cmlb::application::CancelTask& cancel_task;
        cmlb::application::PauseTask& pause_task;
        cmlb::application::ResumeTask& resume_task;
        cmlb::application::UpdateUserSettings& update_user;
        cmlb::application::UpdateBotSettings& update_bot;
        cmlb::application::RssSubscription& rss;
        cmlb::infrastructure::telegram::MessengerInterface& messenger;
        cmlb::infrastructure::system::SystemMetrics& metrics;
        std::chrono::steady_clock::time_point bot_start_time;
    };

    explicit CommandDispatcher(Dependencies deps);
    ~CommandDispatcher();

    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;
    CommandDispatcher(CommandDispatcher&&) = delete;
    CommandDispatcher& operator=(CommandDispatcher&&) = delete;

    /// Dispatches a parsed @p request.
    ///
    /// Behaviour:
    ///  - Unknown commands are silently ignored (returns success).
    ///  - When the sender lacks the required permission, a polite refusal is
    ///    sent via `messenger` and `ErrorCode::PermissionDenied` is returned.
    ///  - Otherwise the registered handler is invoked and its `Result<void>`
    ///    is returned verbatim.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> dispatch(CommandRequest request);

private:
    /// Handler signature for an individual command.
    using Handler = std::function<boost::asio::awaitable<cmlb::core::Result<void>>(CommandRequest)>;

    /// Registered command entry.
    struct Entry {
        cmlb::domain::Permission required;
        Handler handler;
        std::string description;
    };

    /// Populates `commands_` and `aliases_` with the bot's built-in command
    /// set. Invoked exactly once from the constructor.
    void register_builtins_();

    /// Helper used by `register_builtins_` to install a single command. The
    /// @p description is shown in `/help` next to the command name.
    void register_(std::string name,
                   cmlb::domain::Permission required,
                   std::string description,
                   Handler handler);

    /// Helper that wires up an alias pointing at an existing command.
    void register_alias_(std::string alias, std::string canonical);

    Dependencies deps_;
    std::unordered_map<std::string, Entry> commands_;
    std::unordered_map<std::string, std::string> aliases_;
};

} // namespace cmlb::presentation
