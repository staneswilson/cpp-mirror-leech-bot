// SPDX-License-Identifier: MIT
//
// CMLB composition root.
//
// This file constructs every long-lived object the bot needs, wires them
// together, and spawns the asynchronous bot loop. It is intentionally the
// only place in the codebase that knows about every layer simultaneously.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_future.hpp>

#include <cmlb/application/cancel_task.hpp>
#include <cmlb/application/clone_drive_resource.hpp>
#include <cmlb/application/count_drive_resource.hpp>
#include <cmlb/application/delete_drive_resource.hpp>
#include <cmlb/application/leech_url.hpp>
#include <cmlb/application/mirror_url.hpp>
#include <cmlb/application/pause_task.hpp>
#include <cmlb/application/resume_task.hpp>
#include <cmlb/application/rss_subscription.hpp>
#include <cmlb/application/show_status.hpp>
#include <cmlb/application/update_bot_settings.hpp>
#include <cmlb/application/update_user_settings.hpp>
#include <cmlb/core/cancellation.hpp>
#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/authority.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/aria2_downloader.hpp>
#include <cmlb/infrastructure/download/qbittorrent_downloader.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>
#include <cmlb/infrastructure/media/ffmpeg_media_processor.hpp>
#include <cmlb/infrastructure/media/seven_zip_archive_processor.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/rss_feed_repository.hpp>
#include <cmlb/infrastructure/persistence/schema_migrator.hpp>
#include <cmlb/infrastructure/persistence/sqlite_bot_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>
#include <cmlb/infrastructure/persistence/sqlite_rss_feed_repository.hpp>
#include <cmlb/infrastructure/persistence/sqlite_task_repository.hpp>
#include <cmlb/infrastructure/persistence/sqlite_user_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/task_repository.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>
#include <cmlb/infrastructure/rss/rss_feed_poller.hpp>
#include <cmlb/infrastructure/system/signal_handler.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/infrastructure/telegram/authentication_flow.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/telegram/telegram_gateway.hpp>
#include <cmlb/infrastructure/telegram/update_router.hpp>
#include <cmlb/infrastructure/upload/google_drive_uploader.hpp>
#include <cmlb/infrastructure/upload/rclone_uploader.hpp>
#include <cmlb/infrastructure/upload/telegram_uploader.hpp>
#include <cmlb/presentation/callback_dispatcher.hpp>
#include <cmlb/presentation/command_dispatcher.hpp>
#include <cmlb/presentation/command_parser.hpp>
#include <cmlb/presentation/html_renderer.hpp>
#include <cmlb/presentation/progress_renderer.hpp>
#include <cmlb/version.hpp>

namespace {

// ----------------------------------------------------------------------------
// CLI parsing
// ----------------------------------------------------------------------------

struct CliArgs {
    bool show_version{false};
    bool show_help{false};
    bool validate_only{false};
    bool migrate_only{false};
    std::filesystem::path config_path{"config.json"};
};

void print_usage(std::ostream& out) {
    out << "Usage: cmlb [OPTIONS] [CONFIG_PATH]\n"
        << "\n"
        << "CMLB - Production-grade C++23 Telegram mirror/leech bot.\n"
        << "\n"
        << "Arguments:\n"
        << "  CONFIG_PATH               Path to config.json (default: ./config.json)\n"
        << "\n"
        << "Options:\n"
        << "  --version                 Print version and exit.\n"
        << "  --help, -h                Print this help and exit.\n"
        << "  --validate-config <PATH>  Load and validate the config, printing every\n"
        << "                            error if any, then exit. No bot is started.\n"
        << "  --migrate-only            Open the database, apply pending migrations,\n"
        << "                            then exit. Useful for deployment automation.\n"
        << "\n"
        << "Environment variables override matching config fields. The set is\n"
        << "documented in docs/configuration_reference.md.\n";
}

[[nodiscard]] cmlb::core::Result<CliArgs> parse_cli(int argc, char* argv[]) {
    using cmlb::core::error;
    using cmlb::core::ErrorCode;

    CliArgs args;
    std::span<char*> raw{argv + 1, static_cast<std::size_t>(argc - 1)};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::string_view a{raw[i]};
        if (a == "--version") {
            args.show_version = true;
        } else if (a == "--help" || a == "-h") {
            args.show_help = true;
        } else if (a == "--migrate-only") {
            args.migrate_only = true;
        } else if (a == "--validate-config") {
            args.validate_only = true;
            if (i + 1 >= raw.size()) {
                return error(ErrorCode::InvalidArgument,
                             "--validate-config requires a path argument");
            }
            args.config_path = raw[++i];
        } else if (a.starts_with("--")) {
            return error(ErrorCode::InvalidArgument,
                         std::string{"unknown option: "} + std::string{a});
        } else {
            args.config_path = std::string{a};
        }
    }
    return args;
}

// ----------------------------------------------------------------------------
// Logger bring-up helper
// ----------------------------------------------------------------------------

[[nodiscard]] cmlb::core::Result<void> init_logger(const cmlb::core::AppConfig& cfg) {
    cmlb::core::LogConfig log{
        .logs_dir = cfg.logging.logs_dir,
        .level = cfg.logging.level,
        .console = cfg.logging.console,
    };
    return cmlb::core::Logger::initialize(std::move(log));
}

// ----------------------------------------------------------------------------
// Migrate-only path
// ----------------------------------------------------------------------------

[[nodiscard]] int run_migrate_only(const cmlb::core::AppConfig& cfg) {
    namespace asio = boost::asio;
    using cmlb::core::Logger;

    cmlb::core::Executor executor{2};

    try {
        cmlb::infrastructure::persistence::SqliteConnectionPool pool{cfg.database,
                                                                     executor.get_executor()};
        cmlb::infrastructure::persistence::SchemaMigrator migrator{pool};

        auto fut = asio::co_spawn(executor.get_executor(), migrator.migrate(), asio::use_future);
        const auto result = fut.get();
        executor.stop();
        if (!result) {
            Logger::error("Migration failed: {}", result.error().message);
            return EXIT_FAILURE;
        }
        Logger::info("Migrations applied successfully.");
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        Logger::error("Migration aborted: {}", ex.what());
        return EXIT_FAILURE;
    }
}

// ----------------------------------------------------------------------------
// Authority from persisted bot settings
// ----------------------------------------------------------------------------

[[nodiscard]] cmlb::core::Result<cmlb::domain::Authority> build_authority(
    cmlb::infrastructure::persistence::BotSettingsRepository& repo,
    cmlb::core::Executor& executor) {
    namespace asio = boost::asio;
    auto fut = asio::co_spawn(executor.get_executor(), repo.load(), asio::use_future);
    auto settings = fut.get();
    if (!settings) {
        return std::unexpected{std::move(settings.error())};
    }
    // BotSettingsRecord stores `sudo_users`/`authorized_chats` as raw int64_t
    // vectors. Authority wants spans of StrongId<...>. Build the strong-typed
    // arrays here so the rest of the application layer never sees raw IDs.
    std::vector<cmlb::domain::UserId> sudo_users;
    sudo_users.reserve(settings->sudo_users.size());
    for (auto id : settings->sudo_users) {
        sudo_users.emplace_back(id);
    }
    std::vector<cmlb::domain::ChatId> authorized_chats;
    authorized_chats.reserve(settings->authorized_chats.size());
    for (auto id : settings->authorized_chats) {
        authorized_chats.emplace_back(id);
    }
    return cmlb::domain::Authority{
        cmlb::domain::UserId{settings->owner_id},
        std::span<const cmlb::domain::UserId>{sudo_users.data(), sudo_users.size()},
        std::span<const cmlb::domain::ChatId>{authorized_chats.data(), authorized_chats.size()},
    };
}

} // namespace

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(int argc, char* argv[]) try {
    namespace asio = boost::asio;
    using cmlb::core::Logger;

    // ---- 1. Parse CLI ----
    auto cli = parse_cli(argc, argv);
    if (!cli) {
        std::cerr << "Error: " << cli.error().message << "\n\n";
        print_usage(std::cerr);
        return EXIT_FAILURE;
    }
    if (cli->show_help) {
        print_usage(std::cout);
        return EXIT_SUCCESS;
    }
    if (cli->show_version) {
        std::cout << "cmlb " << cmlb::version << "\n";
        return EXIT_SUCCESS;
    }

    // ---- 2. Load + validate config (no logger yet — errors go to stderr) ----
    auto config_result = cmlb::core::Configuration::load(cli->config_path);
    if (!config_result) {
        std::cerr << "Configuration error:\n" << config_result.error().message << "\n";
        return EXIT_FAILURE;
    }
    if (cli->validate_only) {
        std::cout << "Configuration at " << cli->config_path << " is valid.\n";
        return EXIT_SUCCESS;
    }
    const auto& config = *config_result;

    // ---- 3. Logger ----
    if (auto r = init_logger(config); !r) {
        std::cerr << "Logger initialization failed: " << r.error().message << "\n";
        return EXIT_FAILURE;
    }
    Logger::info("CMLB {} starting up.", cmlb::version);

    // ---- 4. Migrate-only short-circuit ----
    if (cli->migrate_only) {
        const int rc = run_migrate_only(config);
        cmlb::core::Logger::shutdown();
        return rc;
    }

    // ---- 5. Core executor + shared utilities ----
    // I/O-bound async work; idle threads cost ~64 KiB of stack each, so we
    // oversubscribe relative to hardware concurrency to keep coroutines moving
    // when one strand is waiting on a syscall (sqlite write, subprocess wait).
    const std::size_t worker_count =
        std::max<std::size_t>(4U, 2U * std::thread::hardware_concurrency());
    cmlb::core::Executor executor{worker_count};
    Logger::info("Executor running with {} worker threads.", worker_count);

    cmlb::infrastructure::system::SystemMetrics metrics;
    cmlb::infrastructure::http::BeastHttpClient http_client{executor.get_executor()};
    cmlb::infrastructure::system::Subprocess subprocess{executor.get_executor()};

    // ---- 6. Persistence ----
    cmlb::infrastructure::persistence::SqliteConnectionPool pool{config.database,
                                                                 executor.get_executor()};
    cmlb::infrastructure::persistence::SchemaMigrator migrator{pool};

    {
        auto fut = asio::co_spawn(executor.get_executor(), migrator.migrate(), asio::use_future);
        if (auto r = fut.get(); !r) {
            Logger::error("Schema migration failed: {}", r.error().message);
            executor.stop();
            return EXIT_FAILURE;
        }
    }

    cmlb::infrastructure::persistence::SqliteTaskRepository tasks_repo{pool};
    cmlb::infrastructure::persistence::SqliteUserSettingsRepository user_settings_repo{pool};
    cmlb::infrastructure::persistence::SqliteBotSettingsRepository bot_settings_repo{pool};
    cmlb::infrastructure::persistence::SqliteRssFeedRepository rss_feed_repo{pool};

    // Seed bot settings from telegram config on first run (owner_id mirrors
    // the JSON-configured owner so an unconfigured DB is still authorised).
    {
        auto fut =
            asio::co_spawn(executor.get_executor(), bot_settings_repo.load(), asio::use_future);
        auto loaded = fut.get();
        if (loaded && loaded->owner_id == 0 && config.telegram.owner_id != 0) {
            loaded->owner_id = config.telegram.owner_id;
            loaded->sudo_users = config.telegram.sudo_users;
            loaded->authorized_chats = config.telegram.authorized_chats;
            auto save_fut = asio::co_spawn(
                executor.get_executor(), bot_settings_repo.save(*loaded), asio::use_future);
            (void)save_fut.get();
        }
    }

    // ---- 7. Telegram stack ----
    cmlb::infrastructure::telegram::TelegramGateway gateway{executor, config.telegram};
    cmlb::infrastructure::telegram::Messenger messenger{gateway};
    cmlb::infrastructure::telegram::UpdateRouter update_router{gateway, executor};
    cmlb::infrastructure::telegram::AuthenticationFlow auth{gateway, config.telegram};

    // ---- 8. Downloaders / uploaders ----
    cmlb::infrastructure::download::Aria2Downloader aria2{executor, config.aria2};
    cmlb::infrastructure::download::QbittorrentDownloader qbit{
        executor, config.qbittorrent, http_client};

    cmlb::infrastructure::upload::TelegramUploader tg_uploader{messenger, config.telegram};
    cmlb::infrastructure::upload::GoogleDriveUploader gdrive_uploader{
        executor, config.google_drive, http_client};
    cmlb::infrastructure::upload::RcloneUploader rclone_uploader{
        executor, config.rclone, subprocess};

    // ---- 9. Media + archive processors (constructed for future wiring) ----
    cmlb::infrastructure::media::FfmpegMediaProcessor ffmpeg_processor{subprocess};
    cmlb::infrastructure::media::SevenZipArchiveProcessor seven_zip_processor{subprocess};
    static_cast<void>(ffmpeg_processor);
    static_cast<void>(seven_zip_processor);

    // ---- 10. Authority + application use cases ----
    auto authority_result = build_authority(bot_settings_repo, executor);
    if (!authority_result) {
        Logger::error("Could not build Authority: {}", authority_result.error().message);
        executor.stop();
        return EXIT_FAILURE;
    }

    // ProgressRenderer owns the per-chat status message and is shared between
    // every use case that pushes progress updates. Constructed here, ahead of
    // the use cases, so it can be ctor-injected into MirrorUrl/LeechUrl.
    const auto bot_start_time = std::chrono::steady_clock::now();
    cmlb::presentation::ProgressRenderer progress_renderer{
        messenger, metrics, bot_start_time, executor.get_executor()};

    // Cross-use-case channel: `/cancel` sets a flag here that the running
    // MirrorUrl / LeechUrl coroutine polls every tick. See
    // include/cmlb/application/active_task_registry.hpp.
    cmlb::application::ActiveTaskRegistry active_tasks;

    cmlb::application::MirrorUrl mirror_url{aria2,
                                            qbit,
                                            gdrive_uploader,
                                            rclone_uploader,
                                            tasks_repo,
                                            user_settings_repo,
                                            messenger,
                                            progress_renderer,
                                            executor,
                                            active_tasks,
                                            config.google_drive.parallel_files_per_directory};
    cmlb::application::LeechUrl leech_url{aria2,
                                          qbit,
                                          tg_uploader,
                                          tasks_repo,
                                          user_settings_repo,
                                          messenger,
                                          progress_renderer,
                                          executor,
                                          active_tasks,
                                          config.telegram.upload_parallelism};
    cmlb::application::CloneDriveResource clone_use_case{
        gdrive_uploader, messenger, config.google_drive.parent_folder_id};
    cmlb::application::CountDriveResource count_use_case{gdrive_uploader, messenger};
    cmlb::application::DeleteDriveResource delete_use_case{gdrive_uploader, messenger};
    cmlb::application::CancelTask cancel_use_case{tasks_repo, aria2, qbit, messenger, active_tasks};
    cmlb::application::PauseTask pause_use_case{tasks_repo, aria2, qbit, messenger};
    cmlb::application::ResumeTask resume_use_case{tasks_repo, aria2, qbit, messenger};
    cmlb::application::ShowStatus show_status_use_case{
        tasks_repo, aria2, qbit, messenger, metrics, bot_start_time};
    cmlb::application::UpdateUserSettings update_user_use_case{user_settings_repo};
    cmlb::application::UpdateBotSettings update_bot_use_case{bot_settings_repo};
    cmlb::application::RssSubscription rss_use_case{rss_feed_repo};

    // ---- 11. Presentation ----
    cmlb::presentation::CommandDispatcher command_dispatcher{
        cmlb::presentation::CommandDispatcher::Dependencies{
            .authority = std::move(*authority_result),
            .mirror_url = mirror_url,
            .leech_url = leech_url,
            .clone = clone_use_case,
            .count = count_use_case,
            .delete_resource = delete_use_case,
            .cancel_task = cancel_use_case,
            .pause_task = pause_use_case,
            .resume_task = resume_use_case,
            .show_status = show_status_use_case,
            .update_user = update_user_use_case,
            .update_bot = update_bot_use_case,
            .rss = rss_use_case,
            .messenger = messenger,
            .metrics = metrics,
            .bot_start_time = bot_start_time,
        }};

    cmlb::presentation::CallbackDispatcher callback_dispatcher{
        cmlb::presentation::CallbackDispatcher::Dependencies{
            .cancel_task = cancel_use_case,
            .pause_task = pause_use_case,
            .resume_task = resume_use_case,
            .update_user = update_user_use_case,
            .messenger = messenger,
        }};

    // ---- 12. RSS poller (auto-enqueues matching entries as mirror tasks) ----
    //
    // `RssFeed` records only the destination chat, so RSS-triggered mirrors
    // are attributed to the bot owner: they count against the owner's quota
    // and surface in `/stats` under that account. Per-user attribution waits
    // on a schema bump (V0004) adding `user_id` to `rss_feeds`.
    const cmlb::domain::UserId rss_owner_attribution{config.telegram.owner_id};
    cmlb::infrastructure::rss::RssFeedPoller rss_poller{
        executor,
        http_client,
        rss_feed_repo,
        [&mirror_url, rss_owner_attribution](
            cmlb::infrastructure::persistence::RssFeed feed,
            cmlb::infrastructure::rss::RssEntry entry) -> asio::awaitable<void> {
            cmlb::application::MirrorRequest request{
                .url = entry.magnet.value_or(entry.torrent_url.value_or(entry.link)),
                .user = rss_owner_attribution,
                .chat = feed.chat,
                .source_message = cmlb::domain::MessageId{0},
                .use_qbittorrent = false,
                .override_destination = std::nullopt,
            };
            auto r = co_await mirror_url.execute(std::move(request));
            if (!r) {
                Logger::warn("RSS auto-mirror for feed {} entry '{}' failed: {}",
                             feed.feed_id,
                             entry.title,
                             r.error().message);
            }
            co_return;
        }};

    // ---- 13. Wire TDLib update handlers through the dispatchers ----
    update_router.on_new_message([&](cmlb::domain::ChatId chat,
                                     cmlb::domain::UserId sender,
                                     cmlb::domain::MessageId msg,
                                     std::string text) -> asio::awaitable<void> {
        auto request = cmlb::presentation::CommandParser::parse(text, sender, chat, msg);
        if (!request)
            co_return;
        auto r = co_await command_dispatcher.dispatch(std::move(*request));
        if (!r) {
            Logger::debug("Command dispatch returned: {}", r.error().message);
        }
        co_return;
    });

    update_router.on_callback_query([&](cmlb::domain::ChatId chat,
                                        cmlb::domain::UserId sender,
                                        cmlb::domain::MessageId msg_id,
                                        cmlb::domain::CallbackQueryId query_id,
                                        std::string data) -> asio::awaitable<void> {
        auto r =
            co_await callback_dispatcher.dispatch(chat, sender, msg_id, query_id, std::move(data));
        if (!r) {
            Logger::debug("Callback dispatch returned: {}", r.error().message);
        }
        co_return;
    });

    update_router.on_file_update([&](cmlb::domain::FileId,
                                     std::int64_t,
                                     std::int64_t,
                                     std::int64_t,
                                     bool,
                                     bool,
                                     bool,
                                     bool) noexcept {
        // File progress events are surfaced through downloader/uploader
        // status polls; the dedicated streaming path lands in v1.1.
    });

    // ---- 14. Signal handling for graceful shutdown ----
    asio::cancellation_signal shutdown_signal;
    cmlb::infrastructure::system::SignalHandler signal_handler{executor.get_executor(),
                                                               shutdown_signal};

    // ---- 15. Spawn long-lived coroutines ----
    auto gateway_run =
        asio::co_spawn(executor.get_executor(),
                       gateway.run(),
                       asio::bind_cancellation_slot(shutdown_signal.slot(), asio::use_future));

    auto auth_run =
        asio::co_spawn(executor.get_executor(),
                       auth.authenticate(),
                       asio::bind_cancellation_slot(shutdown_signal.slot(), asio::use_future));

    auto rss_run =
        asio::co_spawn(executor.get_executor(),
                       rss_poller.run(),
                       asio::bind_cancellation_slot(shutdown_signal.slot(), asio::use_future));

    Logger::info("Bot ready. Awaiting Telegram updates.");

    // ---- 16. Block until a signal arrives ----
    auto signal_future =
        asio::co_spawn(executor.get_executor(), signal_handler.wait_for_signal(), asio::use_future);

    (void)signal_future.get();
    Logger::info("Shutdown requested. Stopping coroutines.");

    // ---- 17. Graceful shutdown ----
    gateway.request_stop();
    shutdown_signal.emit(asio::cancellation_type::terminal);

    (void)gateway_run.wait_for(std::chrono::seconds{15});
    (void)auth_run.wait_for(std::chrono::seconds{2});
    (void)rss_run.wait_for(std::chrono::seconds{2});

    executor.stop();
    cmlb::core::Logger::shutdown();
    return EXIT_SUCCESS;
}
// clang-format off
catch (const std::exception& ex) {
    std::cerr << "Fatal: " << ex.what() << "\n";
    return EXIT_FAILURE;
}
catch (...) {
    std::cerr << "Fatal: unknown exception\n";
    return EXIT_FAILURE;
}
// clang-format on
