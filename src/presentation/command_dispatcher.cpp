// ---------------------------------------------------------------------------
// command_dispatcher.cpp
//
// Wires `CommandRequest`s to use-case calls after consulting `Authority`.
//
// Use-case headers are pulled in here (not in the dispatcher header) so the
// presentation layer header stays free of application-layer details.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/upload_destination.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/google_drive_uploader.hpp>
#include <cmlb/presentation/command_dispatcher.hpp>
#include <cmlb/presentation/html_renderer.hpp>
#include <cmlb/version.hpp>

// Wave 2 application-layer use cases.
#include <cmlb/application/cancel_task.hpp>
#include <cmlb/application/clone_drive_resource.hpp>
#include <cmlb/application/count_drive_resource.hpp>
#include <cmlb/application/delete_drive_resource.hpp>
#include <cmlb/application/leech_url.hpp>
#include <cmlb/application/mirror_url.hpp>
#include <cmlb/application/pause_task.hpp>
#include <cmlb/application/resume_task.hpp>
#include <cmlb/application/rss_subscription.hpp>
#include <cmlb/application/update_bot_settings.hpp>
#include <cmlb/application/update_user_settings.hpp>

namespace cmlb::presentation {

namespace asio = boost::asio;
using cmlb::core::AppError;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;
using cmlb::domain::Permission;

namespace {

/// Splits the first whitespace-delimited token off the front of @p text.
struct SplitResult {
    std::string head;
    std::string tail;
};

[[nodiscard]] SplitResult split_first_token(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto space = text.find_first_of(" \t\r\n", first);
    if (space == std::string_view::npos) {
        return {std::string{text.substr(first)}, {}};
    }
    SplitResult result;
    result.head = std::string{text.substr(first, space - first)};
    const auto next = text.find_first_not_of(" \t\r\n", space);
    if (next != std::string_view::npos) {
        result.tail = std::string{text.substr(next)};
    }
    return result;
}

} // namespace

CommandDispatcher::CommandDispatcher(Dependencies deps) : deps_{std::move(deps)} {
    register_builtins_();
}

CommandDispatcher::~CommandDispatcher() = default;

void CommandDispatcher::register_(std::string name,
                                  Permission required,
                                  std::string description,
                                  Handler handler) {
    commands_.insert_or_assign(std::move(name),
                               Entry{required, std::move(handler), std::move(description)});
}

void CommandDispatcher::register_alias_(std::string alias, std::string canonical) {
    aliases_.insert_or_assign(std::move(alias), std::move(canonical));
}

asio::awaitable<Result<void>> CommandDispatcher::dispatch(CommandRequest request) {
    // Resolve aliases (e.g. "m" -> "mirror").
    std::string resolved_name = request.command;
    if (const auto it = aliases_.find(resolved_name); it != aliases_.end()) {
        resolved_name = it->second;
    }

    const auto entry_it = commands_.find(resolved_name);
    if (entry_it == commands_.end()) {
        Logger::debug("dispatch: unknown command \"{}\" from user {}",
                      request.command,
                      request.sender.value());
        const std::string notice =
            "<b>Unknown command</b> <code>/"
            + HtmlRenderer::escape_html(HtmlRenderer::truncate_for_display(resolved_name, 64))
            + "</code>. Try <code>/help</code>.";
        auto reply = co_await deps_.messenger.send_html(request.chat, notice);
        if (!reply) {
            Logger::warn("dispatch: failed to send unknown-command notice: {}",
                         reply.error().message);
        }
        co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                    "Unknown command: " + resolved_name);
    }

    const Entry& entry = entry_it->second;
    if (!deps_.authority.can_run(request.sender, request.chat, entry.required)) {
        Logger::info("dispatch: permission denied for \"{}\" (user {} chat {})",
                     resolved_name,
                     request.sender.value(),
                     request.chat.value());
        // Best-effort notification — failure of the reply is logged but does
        // not override the PermissionDenied result.
        const auto required_label = [](Permission p) -> std::string_view {
            switch (p) {
            case Permission::Anyone:
                return "anyone";
            case Permission::User:
                return "authorized user";
            case Permission::Admin:
                return "admin (sudo)";
            case Permission::Owner:
                return "owner";
            }
            return "authorized user";
        }(entry.required);
        const std::string notice = "<b>Permission denied.</b> <code>/"
                                   + HtmlRenderer::escape_html(resolved_name)
                                   + "</code> requires <b>" + std::string{required_label} + "</b>.";
        auto reply = co_await deps_.messenger.send_html(request.chat, notice);
        if (!reply) {
            Logger::warn("dispatch: failed to send permission-denied notice: {}",
                         reply.error().message);
        }
        co_return cmlb::core::error(ErrorCode::PermissionDenied,
                                    "Sender lacks permission for command: " + resolved_name);
    }

    co_return co_await entry.handler(std::move(request));
}

// ---------------------------------------------------------------------------
// Built-in command registration.
//
// Each handler is a stateless lambda capturing `this`. Heavy lifting is
// delegated to the injected use case; the dispatcher is responsible only for
// argument parsing and routing.
// ---------------------------------------------------------------------------
void CommandDispatcher::register_builtins_() {
    using namespace cmlb::application;

    // Reusable usage-hint guard: bails early on empty arguments with a
    // friendly hint instead of forwarding "" downstream to the use case
    // where the failure mode is usually a generic "invalid URL" error.
    const auto require_arg = [this](CommandRequest& req,
                                    std::string_view usage_html) -> asio::awaitable<bool> {
        if (!req.arguments.empty())
            co_return true;
        (void)co_await deps_.messenger.send_html(req.chat, std::string{usage_html});
        co_return false;
    };

    // -------- mirror / qbmirror -------------------------------------------
    register_("mirror",
              Permission::User,
              "Download a URL/magnet via aria2 and upload to GDrive/rclone.",
              [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
                  if (!co_await require_arg(
                          req, "<b>Usage:</b> <code>/mirror &lt;url|magnet&gt;</code>")) {
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/mirror requires a url");
                  }
                  MirrorRequest mr{
                      .url = std::move(req.arguments),
                      .user = req.sender,
                      .chat = req.chat,
                      .source_message = req.source_message,
                      .use_qbittorrent = false,
                      .override_destination = std::nullopt,
                  };
                  auto result = co_await deps_.mirror_url.execute(std::move(mr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });
    register_alias_("m", "mirror");

    register_("qbmirror",
              Permission::User,
              "Mirror via qBittorrent (use for torrents you intend to seed).",
              [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
                  if (!co_await require_arg(
                          req, "<b>Usage:</b> <code>/qbmirror &lt;magnet|torrent_url&gt;</code>")) {
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/qbmirror requires a magnet/url");
                  }
                  MirrorRequest mr{
                      .url = std::move(req.arguments),
                      .user = req.sender,
                      .chat = req.chat,
                      .source_message = req.source_message,
                      .use_qbittorrent = true,
                      .override_destination = std::nullopt,
                  };
                  auto result = co_await deps_.mirror_url.execute(std::move(mr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });
    register_alias_("qm", "qbmirror");

    // -------- leech / qbleech ---------------------------------------------
    register_("leech",
              Permission::User,
              "Download via aria2 and upload back to this chat.",
              [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
                  if (!co_await require_arg(
                          req, "<b>Usage:</b> <code>/leech &lt;url|magnet&gt;</code>")) {
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/leech requires a url");
                  }
                  LeechRequest lr{
                      .url = std::move(req.arguments),
                      .user = req.sender,
                      .chat = req.chat,
                      .source_message = req.source_message,
                      .use_qbittorrent = false,
                  };
                  auto result = co_await deps_.leech_url.execute(std::move(lr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });
    register_alias_("l", "leech");

    register_("qbleech",
              Permission::User,
              "Leech via qBittorrent (use for torrents you intend to seed).",
              [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
                  if (!co_await require_arg(
                          req, "<b>Usage:</b> <code>/qbleech &lt;magnet|torrent_url&gt;</code>")) {
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/qbleech requires a magnet/url");
                  }
                  LeechRequest lr{
                      .url = std::move(req.arguments),
                      .user = req.sender,
                      .chat = req.chat,
                      .source_message = req.source_message,
                      .use_qbittorrent = true,
                  };
                  auto result = co_await deps_.leech_url.execute(std::move(lr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });
    register_alias_("ql", "qbleech");

    // -------- clone / count / del -----------------------------------------
    register_(
        "clone",
        Permission::User,
        "Server-side copy a GDrive file or folder to the configured parent.",
        [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
            if (!co_await require_arg(req, "<b>Usage:</b> <code>/clone &lt;drive_url&gt;</code>")) {
                co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                            "/clone requires a drive url");
            }
            CloneRequest cr{
                .source_url = std::move(req.arguments),
                .user = req.sender,
                .chat = req.chat,
            };
            auto result = co_await deps_.clone.execute(std::move(cr));
            if (!result)
                co_return std::unexpected(result.error());
            auto send = co_await deps_.messenger.send_html(
                req.chat, "<b>Clone:</b> <code>" + HtmlRenderer::escape_html(*result) + "</code>");
            if (!send)
                co_return std::unexpected(send.error());
            co_return Result<void>{};
        });

    register_(
        "count",
        Permission::User,
        "Recursively count files/folders/bytes in a GDrive resource.",
        [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
            if (!co_await require_arg(req, "<b>Usage:</b> <code>/count &lt;drive_url&gt;</code>")) {
                co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                            "/count requires a drive url");
            }
            CountRequest cr{
                .source_url = std::move(req.arguments),
                .user = req.sender,
                .chat = req.chat,
            };
            auto result = co_await deps_.count.execute(std::move(cr));
            if (!result)
                co_return std::unexpected(result.error());
            const std::string body = "<b>Count</b>\n"
                                     "<b>Files:</b> <code>"
                                     + std::to_string(result->files)
                                     + "</code>\n"
                                       "<b>Folders:</b> <code>"
                                     + std::to_string(result->folders)
                                     + "</code>\n"
                                       "<b>Total:</b> <code>"
                                     + cmlb::core::format_bytes(result->total_bytes) + "</code>";
            auto send = co_await deps_.messenger.send_html(req.chat, body);
            if (!send)
                co_return std::unexpected(send.error());
            co_return Result<void>{};
        });

    register_(
        "del",
        Permission::Admin,
        "Delete a GDrive file or folder (trash, recoverable for 30 days).",
        [this, require_arg](CommandRequest req) -> asio::awaitable<Result<void>> {
            if (!co_await require_arg(req, "<b>Usage:</b> <code>/del &lt;drive_url&gt;</code>")) {
                co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                            "/del requires a drive url");
            }
            DeleteDriveRequest dr{
                .source_url = std::move(req.arguments),
                .user = req.sender,
                .chat = req.chat,
            };
            auto result = co_await deps_.delete_resource.execute(std::move(dr));
            if (!result)
                co_return std::unexpected(result.error());
            auto send = co_await deps_.messenger.send_html(req.chat, "<b>Deleted.</b>");
            if (!send)
                co_return std::unexpected(send.error());
            co_return Result<void>{};
        });

    // -------- cancel / cancelall ------------------------------------------
    register_("cancel",
              Permission::User,
              "Cancel a running task by id (see /status for ids).",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto split = split_first_token(req.arguments);
                  if (split.head.empty()) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat, "<b>Usage:</b> <code>/cancel &lt;task_id&gt;</code>");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/cancel requires a task id");
                  }
                  CancelTaskRequest ctr{
                      .task_id = cmlb::domain::TaskId{split.head},
                      .chat = req.chat,
                  };
                  auto result = co_await deps_.cancel_task.execute(std::move(ctr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });

    register_("cancelall",
              Permission::Admin,
              "Cancel every running task in this chat.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  auto outcome = co_await deps_.cancel_task.cancel_all(req.chat);
                  if (!outcome) {
                      co_return std::unexpected(outcome.error());
                  }
                  // The use case already sends a summary message; the dispatcher's
                  // job is just to propagate the Result.
                  co_return Result<void>{};
              });

    // -------- pause / resume ----------------------------------------------
    register_("pause",
              Permission::User,
              "Pause a running task by id.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto split = split_first_token(req.arguments);
                  if (split.head.empty()) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat, "<b>Usage:</b> <code>/pause &lt;task_id&gt;</code>");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/pause requires a task id");
                  }
                  PauseTaskRequest pr{
                      .task_id = cmlb::domain::TaskId{split.head},
                      .chat = req.chat,
                  };
                  auto result = co_await deps_.pause_task.execute(std::move(pr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });

    register_("resume",
              Permission::User,
              "Resume a paused task by id.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto split = split_first_token(req.arguments);
                  if (split.head.empty()) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat, "<b>Usage:</b> <code>/resume &lt;task_id&gt;</code>");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "/resume requires a task id");
                  }
                  ResumeTaskRequest rr{
                      .task_id = cmlb::domain::TaskId{split.head},
                      .chat = req.chat,
                  };
                  auto result = co_await deps_.resume_task.execute(std::move(rr));
                  if (!result)
                      co_return std::unexpected(result.error());
                  co_return Result<void>{};
              });

    // -------- settings / botsettings --------------------------------------
    register_("settings",
              Permission::Anyone,
              "View / change your per-user upload preferences.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  // No-op mutator: triggers a load-or-default cycle and returns the
                  // current record without changes.
                  UpdateUserSettingsRequest read_req{
                      .user = req.sender,
                      .mutate =
                          [](auto&) noexcept {
                          },
                  };
                  auto settings = co_await deps_.update_user.execute(std::move(read_req));
                  if (!settings)
                      co_return std::unexpected(settings.error());

                  const std::string body = HtmlRenderer::render_user_settings(*settings);
                  cmlb::infrastructure::telegram::InlineKeyboard kb{
                      {
                          {"Cycle Upload Destination", "settings:upload:cycle"},
                          {"Toggle Document Mode", "settings:doc:toggle"},
                      },
                      {
                          {"Close", "close"},
                      },
                  };
                  auto send = co_await deps_.messenger.send_html_with_keyboard(
                      req.chat, body, std::move(kb));
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    register_("botsettings",
              Permission::Admin,
              "View / change bot-wide configuration (admins only).",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  UpdateBotSettingsRequest read_req{
                      .mutate =
                          [](auto&) noexcept {
                          },
                  };
                  auto settings = co_await deps_.update_bot.execute(std::move(read_req));
                  if (!settings)
                      co_return std::unexpected(settings.error());

                  const std::string body = HtmlRenderer::render_bot_settings(*settings);
                  cmlb::infrastructure::telegram::InlineKeyboard kb{
                      {{"Close", "close"}},
                  };
                  auto send = co_await deps_.messenger.send_html_with_keyboard(
                      req.chat, body, std::move(kb));
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    // -------- help / start ------------------------------------------------
    auto help_handler = [this](CommandRequest req) -> asio::awaitable<Result<void>> {
        // Build a reverse alias map (canonical -> [aliases]) so each
        // entry can show its shortcuts inline.
        std::unordered_map<std::string, std::vector<std::string>> aliases_of;
        for (const auto& [alias, canonical] : aliases_) {
            aliases_of[canonical].push_back(alias);
        }

        std::vector<HtmlRenderer::CommandDescription> descriptions;
        descriptions.reserve(commands_.size());
        for (const auto& [name, entry] : commands_) {
            // Filter: only show commands the caller can actually run.
            // Users in a busy chat shouldn't see owner-only or admin-only
            // commands they cannot exercise — it's noise and a discovery
            // distraction.
            if (!deps_.authority.can_run(req.sender, req.chat, entry.required)) {
                continue;
            }

            std::string desc = entry.description;
            if (auto it = aliases_of.find(name); it != aliases_of.end()) {
                auto& shortcuts = it->second;
                std::ranges::sort(shortcuts);
                desc += " (aliases: ";
                for (std::size_t i = 0; i < shortcuts.size(); ++i) {
                    if (i != 0)
                        desc += ", ";
                    desc += "/";
                    desc += shortcuts[i];
                }
                desc += ")";
            }
            descriptions.push_back({name, std::move(desc), entry.required});
        }
        // Sort: required-permission ascending (Anyone first, Owner last),
        // then alphabetical. Groups the listing by tier so a regular
        // user sees their commands first.
        std::ranges::sort(descriptions, [](const auto& a, const auto& b) {
            if (a.permission != b.permission) {
                return static_cast<int>(a.permission) < static_cast<int>(b.permission);
            }
            return a.name < b.name;
        });
        const std::string body = HtmlRenderer::render_help(descriptions);
        auto send = co_await deps_.messenger.send_html(req.chat, body);
        if (!send)
            co_return std::unexpected(send.error());
        co_return Result<void>{};
    };
    register_("help", Permission::Anyone, "Show this list of commands.", help_handler);
    register_("start",
              Permission::Anyone,
              "Greet the bot and learn what it can do.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  // No first-name is available on the CommandRequest today, so
                  // render_greeting falls back to its generic copy. When the
                  // gateway plumbs through the sender's display name this single
                  // call gains a real personalised hello with no other changes.
                  const std::string body = HtmlRenderer::render_greeting(std::string_view{});
                  auto send = co_await deps_.messenger.send_html(req.chat, body);
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    // -------- stats -------------------------------------------------------
    register_("stats",
              Permission::Anyone,
              "CPU / RAM / disk usage and bot uptime.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto snapshot = deps_.metrics.snapshot();
                  const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - deps_.bot_start_time);
                  const std::string body =
                      HtmlRenderer::render_stats(snapshot, uptime, /*active_downloads=*/0);
                  auto send = co_await deps_.messenger.send_html(req.chat, body);
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    // -------- version -----------------------------------------------------
    register_("version",
              Permission::Anyone,
              "Show the running cmlb build version.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const std::string body =
                      "<b>cmlb</b> v<code>" + std::string{cmlb::version} + "</code>";
                  auto send = co_await deps_.messenger.send_html(req.chat, body);
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    // -------- ping --------------------------------------------------------
    register_("ping",
              Permission::Anyone,
              "Round-trip latency check against Telegram.",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto start = std::chrono::steady_clock::now();
                  auto send = co_await deps_.messenger.send_html(req.chat, "<b>Pong!</b>");
                  if (!send)
                      co_return std::unexpected(send.error());
                  const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start);
                  auto edit = co_await deps_.messenger.edit_html(
                      req.chat,
                      *send,
                      "<b>Pong!</b> Latency: <code>" + std::to_string(latency.count())
                          + " ms</code>");
                  if (!edit)
                      co_return std::unexpected(edit.error());
                  co_return Result<void>{};
              });

    // -------- log ---------------------------------------------------------
    register_("log",
              Permission::Owner,
              "Upload the current cmlb.log file to this chat (owner only).",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const std::filesystem::path log_path{"logs/cmlb.log"};
                  // Size guard: TDLib will happily upload a multi-GB log, but the
                  // operator almost never wants that. 50 MiB matches the rotating
                  // sink's per-file cap so a single rotation segment fits.
                  std::error_code ec;
                  const bool exists = std::filesystem::exists(log_path, ec);
                  if (ec || !exists) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat, "<b>Log:</b> <code>logs/cmlb.log</code> not found.");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::Io,
                                                  "log file not found: " + log_path.string());
                  }
                  std::error_code size_ec;
                  const auto bytes = std::filesystem::file_size(log_path, size_ec);
                  if (size_ec) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat,
                          "<b>Log:</b> couldn't stat <code>logs/cmlb.log</code>: "
                              + HtmlRenderer::escape_html(size_ec.message()) + ".");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::Io,
                                                  "log stat failed: " + size_ec.message());
                  }
                  constexpr std::uint64_t kLogSizeCap = 50ULL * 1024ULL * 1024ULL;
                  if (bytes > kLogSizeCap) {
                      auto send = co_await deps_.messenger.send_html(
                          req.chat,
                          "<b>Log:</b> file is <code>"
                              + cmlb::core::format_bytes(static_cast<std::int64_t>(bytes))
                              + "</code> (over 50 MiB). Truncate or fetch it from disk.");
                      (void)send;
                      co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                  "log file too large to send");
                  }
                  auto send = co_await deps_.messenger.send_file(req.chat,
                                                                 log_path,
                                                                 /*caption=*/"cmlb.log",
                                                                 /*thumbnail=*/std::nullopt);
                  if (!send)
                      co_return std::unexpected(send.error());
                  co_return Result<void>{};
              });

    // -------- rss ---------------------------------------------------------
    register_("rss",
              Permission::User,
              "Manage RSS feeds (subcommands: add, list, remove).",
              [this](CommandRequest req) -> asio::awaitable<Result<void>> {
                  const auto split = split_first_token(req.arguments);
                  const std::string sub = split.head;
                  if (sub == "add") {
                      if (split.tail.empty()) {
                          auto send = co_await deps_.messenger.send_html(
                              req.chat, "<b>Usage:</b> <code>/rss add &lt;url&gt;</code>");
                          (void)send;
                          co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                      "/rss add requires a url");
                      }
                      cmlb::infrastructure::persistence::RssFeed feed;
                      feed.url = split.tail;
                      feed.title = split.tail; // until the poller fetches a title
                      feed.chat = req.chat;
                      feed.enabled = true;

                      auto add_result = co_await deps_.rss.add(std::move(feed));
                      if (!add_result)
                          co_return std::unexpected(add_result.error());
                      auto send = co_await deps_.messenger.send_html(
                          req.chat,
                          "<b>RSS:</b> Subscription added (id <code>" + std::to_string(*add_result)
                              + "</code>).");
                      if (!send)
                          co_return std::unexpected(send.error());
                      co_return Result<void>{};
                  }
                  if (sub == "list") {
                      auto list = co_await deps_.rss.list_for_chat(req.chat);
                      if (!list)
                          co_return std::unexpected(list.error());
                      std::string body = "<b>RSS subscriptions:</b>\n";
                      if (list->empty()) {
                          body += "<i>(none)</i>";
                      } else {
                          for (const auto& feed : *list) {
                              body += "<code>";
                              body += std::to_string(feed.feed_id);
                              body += "</code> ";
                              body += HtmlRenderer::escape_html(feed.title);
                              body += " - <code>";
                              body += HtmlRenderer::escape_html(feed.url);
                              body += "</code>\n";
                          }
                      }
                      auto send = co_await deps_.messenger.send_html(req.chat, body);
                      if (!send)
                          co_return std::unexpected(send.error());
                      co_return Result<void>{};
                  }
                  if (sub == "remove" || sub == "delete" || sub == "del") {
                      const auto id_split = split_first_token(split.tail);
                      if (id_split.head.empty()) {
                          auto send = co_await deps_.messenger.send_html(
                              req.chat, "<b>Usage:</b> <code>/rss remove &lt;id&gt;</code>");
                          (void)send;
                          co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                      "/rss remove requires a feed id");
                      }
                      // C++ coroutines disallow co_await inside catch-handlers,
                      // so flag the parse failure and handle the user-feedback
                      // co_await after the try/catch closes.
                      std::int64_t feed_id = 0;
                      bool parse_failed = false;
                      try {
                          feed_id = std::stoll(id_split.head);
                      } catch (...) {
                          parse_failed = true;
                      }
                      if (parse_failed) {
                          auto send = co_await deps_.messenger.send_html(
                              req.chat, "<b>RSS:</b> Feed id must be a number.");
                          (void)send;
                          co_return cmlb::core::error(ErrorCode::InvalidArgument,
                                                      "feed id is not a number");
                      }
                      auto rem = co_await deps_.rss.remove(feed_id, req.sender, req.chat);
                      if (!rem)
                          co_return std::unexpected(rem.error());
                      auto send = co_await deps_.messenger.send_html(
                          req.chat, "<b>RSS:</b> Subscription removed.");
                      if (!send)
                          co_return std::unexpected(send.error());
                      co_return Result<void>{};
                  }
                  auto send =
                      co_await deps_.messenger.send_html(req.chat,
                                                         "<b>Usage:</b> "
                                                         "<code>/rss add &lt;url&gt;</code>, "
                                                         "<code>/rss list</code>, "
                                                         "<code>/rss remove &lt;id&gt;</code>");
                  (void)send;
                  co_return cmlb::core::error(ErrorCode::InvalidArgument, "unknown rss subcommand");
              });
}

} // namespace cmlb::presentation
