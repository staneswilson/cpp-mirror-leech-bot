#include <cmlb/infrastructure/persistence/schema_migrator.hpp>

#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <sqlite_modern_cpp.h>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

namespace cmlb::infrastructure::persistence {

namespace {

// --------------------------------------------------------------------------
// Embedded SQL. Keep these in sync with the .sql files under migrations/.
// --------------------------------------------------------------------------

constexpr std::string_view kV0001InitialSchemaSql = R"SQL(
-- ---------------------------------------------------------------------------
-- V0001__initial_schema.sql
-- Bootstrap CMLB persistence: version log, bot settings, user settings, tasks.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    applied_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS bot_settings (
    id                    INTEGER PRIMARY KEY CHECK (id = 1),
    owner_id              INTEGER NOT NULL DEFAULT 0,
    sudo_users            TEXT    NOT NULL DEFAULT '[]',
    authorized_chats      TEXT    NOT NULL DEFAULT '[]',
    download_dir          TEXT    NOT NULL DEFAULT 'downloads',
    leech_split_size      INTEGER NOT NULL DEFAULT 2097152000,
    upload_limit_bytes    INTEGER NOT NULL DEFAULT 0,
    status_update_interval_ms INTEGER NOT NULL DEFAULT 5000,
    rss_poll_interval_ms  INTEGER NOT NULL DEFAULT 60000,
    updated_at            TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS user_settings (
    user_id              INTEGER PRIMARY KEY,
    leech_destination    TEXT    NOT NULL DEFAULT 'telegram',
    mirror_destination   TEXT    NOT NULL DEFAULT 'gdrive',
    default_thumb_path   TEXT,
    rclone_remote        TEXT,
    gdrive_folder_id     TEXT,
    upload_as_document   INTEGER NOT NULL DEFAULT 0,
    created_at           TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at           TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS tasks (
    id                  TEXT    PRIMARY KEY,
    user_id             INTEGER NOT NULL,
    chat_id             INTEGER NOT NULL,
    status_message_id   INTEGER NOT NULL,
    kind                TEXT    NOT NULL,
    state               TEXT    NOT NULL,
    source_url          TEXT    NOT NULL,
    error_message       TEXT,
    created_at          TEXT    NOT NULL,
    updated_at          TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_tasks_user_id ON tasks(user_id);
CREATE INDEX IF NOT EXISTS idx_tasks_state   ON tasks(state);
)SQL";

constexpr std::string_view kV0002RssFeedsSql = R"SQL(
-- ---------------------------------------------------------------------------
-- V0002__rss_feeds.sql
-- RSS feed subscriptions polled by the RSS monitor subsystem.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS rss_feeds (
    feed_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title            TEXT    NOT NULL,
    url              TEXT    NOT NULL UNIQUE,
    chat_id          INTEGER NOT NULL,
    include_regex    TEXT    NOT NULL DEFAULT '',
    exclude_regex    TEXT    NOT NULL DEFAULT '',
    last_guid        TEXT    NOT NULL DEFAULT '',
    last_checked_at  TEXT,
    enabled          INTEGER NOT NULL DEFAULT 1,
    created_at       TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_rss_feeds_enabled ON rss_feeds(enabled);
)SQL";

constexpr std::string_view kV0003TaskDownloaderAttachmentSql = R"SQL(
-- ---------------------------------------------------------------------------
-- V0003__task_downloader_attachment.sql
-- Persist the downloader binding (Gid + kind) per Task so that /cancel,
-- /pause, /resume can dispatch to the correct downloader without guessing.
-- ---------------------------------------------------------------------------

ALTER TABLE tasks ADD COLUMN downloader_kind TEXT NOT NULL DEFAULT 'None';
ALTER TABLE tasks ADD COLUMN downloader_id   TEXT;

CREATE INDEX IF NOT EXISTS idx_tasks_downloader_id
    ON tasks(downloader_id)
 WHERE downloader_id IS NOT NULL;
)SQL";

const std::vector<Migration>& migrations() {
    static const std::vector<Migration> kRegistry{
        {1, "V0001__initial_schema",            kV0001InitialSchemaSql},
        {2, "V0002__rss_feeds",                 kV0002RssFeedsSql},
        {3, "V0003__task_downloader_attachment", kV0003TaskDownloaderAttachmentSql},
    };
    return kRegistry;
}

[[nodiscard]] core::Result<void> ensure_schema_version_table(sqlite::database& db) {
    try {
        db << R"SQL(
            CREATE TABLE IF NOT EXISTS schema_version (
                version    INTEGER PRIMARY KEY,
                name       TEXT NOT NULL,
                applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            );
        )SQL";
        return {};
    } catch (const sqlite::sqlite_exception& ex) {
        return core::error(
            core::ErrorCode::Migration,
            std::string{"Failed to ensure schema_version table: "} + ex.what());
    }
}

[[nodiscard]] core::Result<int> read_current_version(sqlite::database& db) {
    try {
        int max_version = 0;
        bool any_row    = false;
        db << "SELECT COALESCE(MAX(version), 0) FROM schema_version;"
           >> [&](int v) {
               max_version = v;
               any_row     = true;
           };
        return any_row ? max_version : 0;
    } catch (const sqlite::sqlite_exception& ex) {
        return core::error(core::ErrorCode::Database,
                           std::string{"Failed to read schema_version: "} + ex.what());
    }
}

[[nodiscard]] core::Result<void> apply_one(sqlite::database& db, const Migration& m) {
    try {
        db << "BEGIN IMMEDIATE;";
    } catch (const sqlite::sqlite_exception& ex) {
        return core::error(
            core::ErrorCode::Migration,
            std::string{"Failed to BEGIN for migration "} + std::string{m.name} + ": "
                + ex.what());
    }

    try {
        // sqlite-modern-cpp does not execute multi-statement SQL via operator<<,
        // so use the C API exec for the migration body.
        char* err_msg = nullptr;
        const int rc  = sqlite3_exec(db.connection().get(),
                                    std::string{m.sql}.c_str(),
                                    nullptr,
                                    nullptr,
                                    &err_msg);
        if (rc != SQLITE_OK) {
            std::string detail = err_msg != nullptr ? err_msg : "(no detail)";
            if (err_msg != nullptr) {
                sqlite3_free(err_msg);
            }
            db << "ROLLBACK;";
            return core::error(
                core::ErrorCode::Migration,
                std::string{"Migration "} + std::string{m.name} + " failed: " + detail);
        }

        db << "INSERT INTO schema_version (version, name) VALUES (?, ?);"
           << m.version
           << std::string{m.name};

        db << "COMMIT;";
        return {};
    } catch (const sqlite::sqlite_exception& ex) {
        try {
            db << "ROLLBACK;";
        } catch (...) {
            // Already failing — swallow to surface the original error.
        }
        return core::error(
            core::ErrorCode::Migration,
            std::string{"Migration "} + std::string{m.name} + " threw: " + ex.what());
    }
}

}  // namespace

const std::vector<Migration>& SchemaMigrator::registry() noexcept {
    return migrations();
}

boost::asio::awaitable<core::Result<int>> SchemaMigrator::current_version() {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }

    auto& db = acquired->database();
    if (auto ensured = ensure_schema_version_table(db); !ensured.has_value()) {
        co_return std::unexpected{ensured.error()};
    }
    co_return read_current_version(db);
}

boost::asio::awaitable<core::Result<void>> SchemaMigrator::migrate() {
    auto acquired = co_await pool_.acquire();
    if (!acquired.has_value()) {
        co_return std::unexpected{acquired.error()};
    }
    auto& db = acquired->database();

    if (auto ensured = ensure_schema_version_table(db); !ensured.has_value()) {
        co_return std::unexpected{ensured.error()};
    }

    auto current = read_current_version(db);
    if (!current.has_value()) {
        co_return std::unexpected{current.error()};
    }

    for (const auto& m : migrations()) {
        if (m.version <= *current) {
            continue;
        }
        if (auto applied = apply_one(db, m); !applied.has_value()) {
            co_return std::unexpected{applied.error()};
        }
    }
    co_return core::Result<void>{};
}

}  // namespace cmlb::infrastructure::persistence
