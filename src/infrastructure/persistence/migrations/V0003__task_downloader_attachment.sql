-- ---------------------------------------------------------------------------
-- V0003__task_downloader_attachment.sql
--
-- Documentation copy of the V3 migration body. The authoritative SQL is
-- compiled into src/infrastructure/persistence/schema_migrator.cpp as a raw
-- string literal — keep this file in sync with the embedded copy.
--
-- Persists the (Gid, DownloaderKind) binding per Task so /cancel, /pause,
-- and /resume dispatch to exactly the downloader that accepted the job.
-- Rows with downloader_kind = 'None' are treated as orphan and reject those
-- operations with InvalidState.
-- ---------------------------------------------------------------------------

ALTER TABLE tasks ADD COLUMN downloader_kind TEXT NOT NULL DEFAULT 'None';
ALTER TABLE tasks ADD COLUMN downloader_id   TEXT;

CREATE INDEX IF NOT EXISTS idx_tasks_downloader_id
    ON tasks(downloader_id)
 WHERE downloader_id IS NOT NULL;
