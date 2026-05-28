-- ---------------------------------------------------------------------------
-- V0002__rss_feeds.sql
--
-- Documentation copy of the V2 migration body. Keep in sync with the
-- raw-string-literal in src/infrastructure/persistence/schema_migrator.cpp.
--
-- Adds the rss_feeds table backing the RSS monitor subsystem. Each row is a
-- single feed subscription; the include/exclude regex fields gate which entries
-- are turned into mirror/leech tasks. `last_guid` and `last_checked_at` are
-- updated by the poller so duplicate entries are skipped across restarts.
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
