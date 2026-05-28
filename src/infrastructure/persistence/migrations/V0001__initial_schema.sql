-- ---------------------------------------------------------------------------
-- V0001__initial_schema.sql
--
-- Documentation copy of the V1 migration body. The authoritative SQL is
-- compiled into src/infrastructure/persistence/schema_migrator.cpp as a raw
-- string literal. Keep this file in sync with the embedded copy.
--
-- Tables:
--   * schema_version  — append-only ledger of applied migrations.
--   * bot_settings    — singleton (id = 1) carrying global bot configuration
--                       that can be tweaked at runtime by the owner.
--   * user_settings   — per-user preferences keyed by Telegram user_id.
--   * tasks           — persisted Task aggregates (one row per Task).
--
-- Indexes accelerate the two access patterns we care about: "all tasks for a
-- given user" and "all non-terminal tasks for the dispatcher".
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
