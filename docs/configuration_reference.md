# CMLB configuration reference

This document describes every field accepted in `config.json`, its type, default, and meaning. The configuration is parsed once at startup; restart the bot to apply changes (live reload is not supported in v1).

For higher-level guidance on *which* fields to set first, see [`runbook.md`](runbook.md). For the rationale of why some defaults look the way they do, see [`architecture.md`](architecture.md).

---

## Table of contents

- [Loading order and overrides](#loading-order-and-overrides)
- [Validation](#validation)
- [Top-level layout](#top-level-layout)
- [`telegram`](#telegram)
- [`aria2`](#aria2)
- [`qbittorrent`](#qbittorrent)
- [`rclone`](#rclone)
- [`google_drive`](#google_drive)
- [`database`](#database)
- [`logging`](#logging)
- [`paths`](#paths)
- [`executor`](#executor)
- [`metrics`](#metrics)
- [`rss`](#rss)
- [`limits`](#limits)
- [Environment variable overrides](#environment-variable-overrides)
- [A minimal config](#a-minimal-config)
- [A fully-populated config](#a-fully-populated-config)

---

## Loading order and overrides

1. CMLB reads the JSON file at `--config <path>`, falling back to `$CMLB_CONFIG_PATH`, then `./config.json`.
2. For each leaf field, CMLB looks up the matching environment variable (`CMLB_<UPPER_SNAKE_PATH>`). If set, the env variable overrides the JSON value.
3. The merged result is validated. All errors are collected and reported together.
4. The result is frozen into an immutable `core::Configuration` and passed via dependency injection.

`--validate-config` runs steps 1-3 and exits without starting the bot.

---

## Validation

Validation is strict and collected:

- Required fields with no default produce `field missing`.
- Type mismatches produce `field expected <type>, got <type>`.
- Range violations (e.g. negative `port`) produce `field out of range`.
- Cross-field rules (e.g. `google_drive` enabled but `service_account_path` missing) produce `field requires <other field>`.

A failing validation reports *every* problem at once, not just the first. This makes one-pass operator fixes possible.

---

## Top-level layout

```json
{
  "telegram":     { ... },
  "aria2":        { ... },
  "qbittorrent":  { ... },
  "rclone":       { ... },
  "google_drive": { ... },
  "database":     { ... },
  "logging":      { ... },
  "paths":        { ... },
  "executor":     { ... },
  "metrics":      { ... },
  "rss":          { ... },
  "limits":       { ... }
}
```

Sections marked `optional` may be omitted entirely; defaults apply to every absent field.

---

## `telegram`

Required. Defines how CMLB talks to Telegram via TDLib.

| Field | Type | Default | Description |
|---|---|---|---|
| `api_id` | int32 | required | App `api_id` from <https://my.telegram.org/apps>. Identifies your application to Telegram. |
| `api_hash` | string | required | App `api_hash` from the same page. Treat as a secret. |
| `bot_token` | string | required for bot mode | Bot token issued by @BotFather (format `123:AAH...`). Mutually exclusive with `phone_number`. |
| `phone_number` | string | required for user mode | E.164 phone number for user-account mode (e.g. `+15551234567`). Mutually exclusive with `bot_token`. |
| `owner_id` | int64 | required | Numeric user id of the bot owner. Bypasses all permission checks. |
| `admins` | array of int64 | `[]` | Numeric user ids granted the `Admin` authority tier. |
| `users` | array of int64 | `[]` | Numeric user ids granted the `User` authority tier. If empty, anyone can issue `User`-tier commands. |
| `allowed_chats` | array of int64 | `[]` | If non-empty, CMLB only responds in these chat ids. The bot still receives updates in other chats but ignores commands. |
| `tdlib_directory` | string | `paths.data + "/tdlib"` | Where TDLib persists session data. |
| `tdlib_log_verbosity` | int (0-10) | 1 | TDLib's own log verbosity. 0 silences TDLib; 5+ is noisy. |
| `tdlib_use_test_dc` | bool | `false` | Use Telegram's test data centre. For development only. |
| `progress_edit_interval_ms` | uint32 | 2000 | Minimum interval between progress-message edits per chat. Telegram caps edits at ~1/sec/chat. |
| `upload_split_bytes` | uint64 | 2_000_000_000 | Maximum bytes per Telegram document. Files larger than this are split. Set to 4_000_000_000 if the bot account has Premium. |
| `parse_mode` | enum | `"html"` | Outgoing message parse mode. One of `"html"` or `"markdown"`. |

> **Warning:** `api_hash` and `bot_token` are secrets. Do not commit `config.json`. Do not paste these in chat logs or screenshots.

---

## `aria2`

Optional. Required only if any of the `aria2*` use cases is invoked. Default downloader for most workflows.

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `true` | If `false`, the aria2 adapter is not registered; commands that need it return `External unavailable`. |
| `rpc_url` | string | `"ws://127.0.0.1:6800/jsonrpc"` | WebSocket JSON-RPC endpoint of aria2c. `ws://` or `wss://`. |
| `rpc_secret` | string | `""` | Token configured via `--rpc-secret` on the aria2c side. Sent as `token:<secret>`. |
| `connect_timeout_ms` | uint32 | 10_000 | Timeout for the initial WebSocket handshake. |
| `request_timeout_ms` | uint32 | 30_000 | Timeout for each individual JSON-RPC call. |
| `max_concurrent_downloads` | uint32 | 4 | Soft cap on simultaneous aria2 jobs CMLB will start. aria2's own `max-concurrent-downloads` still applies. |
| `max_connection_per_server` | uint32 | 8 | Forwarded to aria2 as `max-connection-per-server` when starting a download. |
| `split` | uint32 | 8 | Forwarded to aria2 as `split`. |
| `min_split_size` | string | `"10M"` | Forwarded to aria2 as `min-split-size`. Accepts SI suffixes (K/M/G). |
| `seed_time_minutes` | uint32 | 0 | After a torrent download finishes, seed for this many minutes. 0 disables seeding. |
| `seed_ratio` | float | 1.0 | Stop seeding when ratio reaches this value (ignored if `seed_time_minutes` is 0). |
| `bt_tracker` | array of string | `[]` | Extra BitTorrent trackers to inject into magnets. |

---

## `qbittorrent`

Optional. Required only for `/qbmirror` and `/qbleech`. Used when the qBittorrent feature set (DHT, specific tracker handling, etc.) is preferable to aria2.

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | If `false`, qBittorrent commands are not registered. |
| `base_url` | string | `"http://127.0.0.1:8080"` | qBittorrent Web UI URL. CMLB authenticates against `/api/v2/auth/login`. |
| `username` | string | required if enabled | Web UI username. |
| `password` | string | required if enabled | Web UI password. |
| `category` | string | `"cmlb"` | qBittorrent category applied to torrents added by CMLB. Useful for filtering in the Web UI. |
| `save_path` | string | `paths.downloads` | Per-torrent `savepath` sent on add. |
| `seed_time_minutes` | uint32 | 0 | Maximum seeding time. 0 = forever (qBittorrent default). |
| `seed_ratio` | float | -1.0 | Maximum seeding ratio. -1 = unlimited. |
| `request_timeout_ms` | uint32 | 30_000 | Per-request HTTP timeout. |
| `verify_tls` | bool | `true` | If `base_url` is `https`, verify the server certificate. |

---

## `rclone`

Optional. Required only if `mirror` is allowed to upload to rclone remotes.

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | If `false`, rclone destinations are not selectable. |
| `binary` | string | `"rclone"` | Path to the `rclone` binary. Resolved via `$PATH` if not absolute. |
| `config_path` | string | `"~/.config/rclone/rclone.conf"` | rclone config file. Passed as `--config <path>`. |
| `default_remote` | string | `""` | Remote prefix used when the user does not specify one (e.g. `gdrive:Backups`). |
| `extra_args` | array of string | `[]` | Extra flags appended to every `rclone copy` invocation (e.g. `["--transfers", "4"]`). |
| `verify_tls` | bool | `true` | If `false`, passes `--no-check-certificate`. Not recommended. |

---

## `google_drive`

Optional. Required only for `/clone`, `/count`, `/del`, and mirror operations targeting Drive directly (without going through rclone).

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | If `false`, Drive adapters are not registered. |
| `service_account_path` | string | required if enabled | Path to the service-account JSON key file. |
| `default_folder_id` | string | `""` | Drive folder id where mirrored uploads go by default. Must be shared with the service account as Editor. |
| `shared_drive_id` | string | `""` | If using a Shared Drive, its id. Mirrors land under the Shared Drive's root if `default_folder_id` is empty. |
| `use_resumable_upload_threshold_bytes` | uint64 | 5_000_000 | Files smaller than this use a single PUT; larger files use a resumable session. |
| `chunk_size_bytes` | uint32 | 8_388_608 | Resumable upload chunk size. Must be a multiple of 256 KiB. |
| `request_timeout_ms` | uint32 | 60_000 | Per-request HTTP timeout. |
| `index_url` | string | `""` | Optional Drive index URL. If set, `/clone` results include an index-friendly view link. |

---

## `database`

Optional. Defaults are sane for a single-instance deployment.

| Field | Type | Default | Description |
|---|---|---|---|
| `path` | string | `paths.data + "/cmlb.db"` | SQLite database file. Created on first run. |
| `connection_pool_size` | uint32 | 4 | Number of read connections kept in the pool. The single writer connection is separate. |
| `busy_timeout_ms` | uint32 | 5000 | `PRAGMA busy_timeout`. With WAL mode this should rarely trigger. |
| `journal_mode` | enum | `"wal"` | Journal mode. One of `"wal"`, `"delete"`, `"truncate"`, `"persist"`, `"memory"`. Do not change without a strong reason. |
| `synchronous` | enum | `"normal"` | `PRAGMA synchronous`. One of `"off"`, `"normal"`, `"full"`, `"extra"`. `"normal"` survives application crashes; `"off"` does not. |
| `foreign_keys` | bool | `true` | `PRAGMA foreign_keys = ON`. Required for schema integrity. |
| `migrate_on_start` | bool | `true` | If `false`, the bot refuses to start with an out-of-date schema and exits with a message. Useful in environments where migrations must be operator-initiated. |

---

## `logging`

Optional.

| Field | Type | Default | Description |
|---|---|---|---|
| `level` | enum | `"info"` | One of `"trace"`, `"debug"`, `"info"`, `"warn"`, `"error"`, `"critical"`, `"off"`. |
| `format` | enum | `"text"` | `"text"` or `"json"`. JSON mode emits one object per line for ingestion by log aggregators. |
| `file` | string | `paths.data + "/cmlb.log"` | Log file path. Empty string disables file logging. |
| `file_max_size_bytes` | uint64 | 10_485_760 | Rotate the file when it exceeds this size. |
| `file_max_files` | uint32 | 5 | Number of historical rotated files to retain. |
| `console` | bool | `true` | Mirror log output to stderr. |
| `redact_secrets` | bool | `true` | Redact known-sensitive fields (`api_hash`, `bot_token`, `service_account`, `rpc_secret`) in every log line. Do not disable in production. |
| `module_levels` | object | `{}` | Map of logger-name → level for per-module overrides. Example: `{ "aria2": "debug" }`. |

---

## `paths`

Optional. Where CMLB puts state on disk.

| Field | Type | Default | Description |
|---|---|---|---|
| `downloads` | string | `"./downloads"` | Working directory for downloaded files. Cleaned after a successful task by default. |
| `data` | string | `"./data"` | Long-lived state: the SQLite database, logs, TDLib session. |
| `tmp` | string | system temp | Scratch space for archive extraction, ffmpeg transcoding, etc. |
| `keep_downloads_on_success` | bool | `false` | If `true`, downloaded files are not deleted after a successful mirror/leech. |
| `keep_downloads_on_failure` | bool | `true` | If `true`, partial downloads survive a failed task for forensic inspection. |

> **Note:** All paths are resolved relative to the process working directory at startup. Prefer absolute paths in production.

---

## `executor`

Optional. Async runtime tuning. Defaults are recommended for almost all deployments.

| Field | Type | Default | Description |
|---|---|---|---|
| `worker_threads` | uint32 | `min(hardware_concurrency, 8)` | Number of `io_context` worker threads. |
| `blocking_pool_threads` | uint32 | 4 | Dedicated thread pool for blocking work (subprocess wait, large filesystem traversal). |
| `cancellation_grace_ms` | uint32 | 5000 | When shutting down, wait this long for in-flight coroutines to honour cancellation before forced exit. |

---

## `metrics`

Optional. Prometheus metrics endpoint. Off by default.

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | If `true`, exposes a `/metrics` endpoint. |
| `bind` | string | `"127.0.0.1:9464"` | `host:port` to bind the metrics HTTP server. **Keep on `127.0.0.1` unless fronted by an authenticating reverse proxy.** |
| `path` | string | `"/metrics"` | URL path of the scrape endpoint. |

---

## `rss`

Optional. RSS subscription polling.

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `true` | If `false`, `/rss` commands are not registered. |
| `default_interval_seconds` | uint32 | 600 | Default poll interval for newly-added feeds. Overridable per feed. |
| `min_interval_seconds` | uint32 | 60 | Lower bound. Operators cannot create feeds polled more frequently than this. |
| `user_agent` | string | `"CMLB/0.1 (+https://github.com/staneswilson/cpp-mirror-leech-bot)"` | `User-Agent` header sent on feed fetches. |
| `request_timeout_ms` | uint32 | 30_000 | HTTP timeout per feed fetch. |
| `max_entries_per_poll` | uint32 | 25 | Soft cap on how many new entries are processed in one poll. Prevents a misconfigured feed from flooding the bot. |

---

## `limits`

Optional. Per-user and global guard rails.

| Field | Type | Default | Description |
|---|---|---|---|
| `max_concurrent_tasks_per_user` | uint32 | 2 | A single user can have at most this many active tasks. `Admin` and `Owner` tiers bypass this limit. |
| `max_concurrent_tasks_global` | uint32 | 16 | Hard cap on simultaneous active tasks across the bot. Excess commands queue. |
| `max_filesize_bytes` | uint64 | 0 | Reject downloads larger than this. 0 disables the check. |
| `daily_quota_bytes_per_user` | uint64 | 0 | Reject when a user's last-24h aggregate download exceeds this. 0 disables. |
| `command_cooldown_ms` | uint32 | 1000 | Minimum gap between accepted commands from the same user. Hits below this are silently ignored. |

---

## Throughput tunables (implemented in code)

The sections above are the long-term canonical reference; some of their entries describe future fields not yet wired to code. The fields documented below are **the ones currently honored** by `core/configuration.hpp` and `core/configuration.cpp` — they correspond to PR 4 of the throughput initiative and are read by the aria2 / qBittorrent / rclone / Google Drive / Telegram code paths.

All sizes use aria2/rclone size syntax (`1M`, `512K`, `64M`, ...). All durations are integers in the unit named by the field (seconds, milliseconds, minutes — see below).

### `telegram` — TDLib tuning

| Field | Default | Unit / Range | Notes |
|---|---|---|---|
| `upload_chunk_size_kb` | `2048` | KiB, `[1, 65536]` | TDLib `upload_chunk_size_kb` option. Larger = fewer round-trips per file. TDLib clamps server-side. |
| `download_chunk_size_kb` | `1024` | KiB, `[1, 65536]` | TDLib `download_chunk_size_kb` option. |
| `connection_retry_count_max` | `5` | int, `>= 0` | TDLib `connection_retry_count_max`. Number of reconnect attempts before giving up on a stale connection. |
| `prefer_ipv6` | `false` | bool | Prefer IPv6 endpoints when both are available. |
| `upload_parallelism` | `4` | int, `[1, 32]` | Parts of a split file kept in flight by `TelegramUploader`. Each part is a separate `messages.sendMedia`. |
| `upload_files_parallelism` | `2` | int, `[1, 32]` | Distinct files kept in flight by `TelegramUploader::upload_directory`. TDLib pipelines its own upload sessions, so concurrent `send_file` calls saturate the link more than a strict for-each loop. |

### `aria2` — daemon options pushed via `aria2.changeGlobalOption`

| Field | Default | Unit / Range | Notes |
|---|---|---|---|
| `max_connection_per_server` | `16` | int, `[1, 16]` | aria2's hard upper bound is 16. |
| `split` | `16` | int, `[1, 16]` | Per-task connection split. Hard upper bound 16. |
| `min_split_size` | `"1M"` | aria2 size syntax | Smaller pieces increase per-piece overhead; larger pieces reduce parallelism on small files. |
| `disk_cache` | `"128M"` | aria2 size syntax | RAM cache that absorbs disk write bursts. |
| `max_tries` | `5` | int, `>= 0` | Per-URL retry count. |
| `retry_wait` | `5` | seconds, `>= 0` | Wait between retries. |
| `max_overall_download_limit` | `0` | bytes/sec, `>= 0` | `0` = unlimited. |
| `max_overall_upload_limit` | `0` | bytes/sec, `>= 0` | `0` = unlimited. |
| `enable_dht` | `true` | bool | BitTorrent DHT. |
| `enable_pex` | `true` | bool | BitTorrent Peer Exchange. |
| `bt_max_peers` | `55` | int, `>= 1` | Maximum peers per torrent. |
| `user_agent` | `"aria2/1.37.0"` | string | Reported on HTTP(S) downloads. |

### `qbittorrent` — preferences pushed via `POST /api/v2/app/setPreferences`

These are sent immediately after a successful Web-UI login. qBittorrent silently ignores unknown keys on older builds; failures are logged at `warn` and are not fatal.

| Field | Default | Unit / Range | qBit preference key |
|---|---|---|---|
| `max_active_downloads` | `8` | int, `>= 1` | `max_active_downloads` |
| `max_active_uploads` | `8` | int, `>= 1` | `max_active_uploads` |
| `max_active_torrents` | `16` | int, `>= 1` | `max_active_torrents` |
| `max_connections` | `500` | int, `>= 1` | `max_connec` (global) |
| `max_connections_per_torrent` | `100` | int, `>= 1` | `max_connec_per_torrent` |
| `max_uploads` | `20` | int, `>= 1` | `max_uploads` (global slots) |
| `max_uploads_per_torrent` | `5` | int, `>= 1` | `max_uploads_per_torrent` |
| `up_limit` | `0` | bytes/sec | `-1` unlimited; `0` leaves qBit default. |
| `dl_limit` | `0` | bytes/sec | `-1` unlimited; `0` leaves qBit default. |
| `dht`, `pex`, `lsd` | `true` | bool | BitTorrent discovery. |
| `anonymous_mode` | `false` | bool | Strips client identity from peer traffic. |
| `async_io_threads` | `8` | int, `>= 1` | qBit's I/O worker pool. |
| `disk_cache_mib` | `256` | MiB, `-1` or `>= 0` | `-1` lets qBit auto-size. |

### `rclone` — flags passed to every `rclone copy` / `rclone sync`

| Field | Default | Unit / Range | rclone flag |
|---|---|---|---|
| `transfers` | `8` | int, `>= 1` | `--transfers` |
| `checkers` | `16` | int, `>= 1` | `--checkers` |
| `multi_thread_streams` | `4` | int, `>= 1` | `--multi-thread-streams` |
| `multi_thread_cutoff` | `"250M"` | rclone size | `--multi-thread-cutoff` |
| `drive_chunk_size` | `"64M"` | rclone size | `--drive-chunk-size` (clamped to 256K alignment) |
| `buffer_size` | `"32M"` | rclone size | `--buffer-size`. **Per-transfer.** RAM cost = `transfers × buffer_size`. Tune down on low-RAM hosts. |
| `use_mmap` | `true` | bool | `--use-mmap` (lower RSS, faster on large files) |
| `fast_list` | `true` | bool | `--fast-list` (single API listing per directory tree) |
| `drive_acknowledge_abuse` | `true` | bool | `--drive-acknowledge-abuse` (allow downloads flagged as abusive) |
| `log_level` | `"NOTICE"` | enum | `--log-level`. One of `DEBUG`, `INFO`, `NOTICE`, `ERROR`. |
| `extra_args` | `[]` | array of string | Appended verbatim. Escape hatch for one-off flags. |

### `google_drive` — resumable-upload parallelism

| Field | Default | Unit / Range | Notes |
|---|---|---|---|
| `parallel_chunks_per_file` | `4` | int, `[1, 64]` | Concurrent chunk PUTs against a single resumable session URI. GDrive accepts out-of-order ranges. |
| `parallel_files_per_directory` | `4` | int, `[1, 64]` | Files uploaded in parallel by `upload_directory`. |
| `max_retries` | `6` | int, `>= 0` | Max attempts on 429 / 5xx, with exponential backoff. |
| `initial_retry_delay` | `500` | ms, `>= 0` | First retry delay; doubles each attempt. |

> **Tuning guidance.** Defaults aim for a gigabit link with reasonable RAM (a 32 MiB rclone `buffer_size × 8 transfers = 256 MiB` ceiling). On a 100 Mbps link, lowering `parallel_chunks_per_file` to 2 and `rclone.transfers` to 4 will saturate the link without burning per-connection overhead. On constrained RAM (1 GiB containers), drop `rclone.buffer_size` to `"8M"` first — it dominates RSS.

---

## Environment variable overrides

Every leaf field is overridable via an environment variable. The mapping is mechanical:

1. Join the field path with underscores.
2. Uppercase everything.
3. Prepend `CMLB_`.

Examples:

| JSON path | Env variable |
|---|---|
| `telegram.api_id` | `CMLB_TELEGRAM_API_ID` |
| `telegram.bot_token` | `CMLB_TELEGRAM_BOT_TOKEN` |
| `aria2.rpc_secret` | `CMLB_ARIA2_RPC_SECRET` |
| `google_drive.service_account_path` | `CMLB_GOOGLE_DRIVE_SERVICE_ACCOUNT_PATH` |
| `logging.level` | `CMLB_LOGGING_LEVEL` |
| `paths.downloads` | `CMLB_PATHS_DOWNLOADS` |
| `metrics.enabled` | `CMLB_METRICS_ENABLED` |
| `limits.max_concurrent_tasks_global` | `CMLB_LIMITS_MAX_CONCURRENT_TASKS_GLOBAL` |

Array fields cannot currently be set via environment variables; use the JSON file for those.

Booleans accept `"true"`/`"false"`, `"1"`/`"0"`, `"yes"`/`"no"` (case-insensitive). Numbers accept decimal integers; sizes with suffixes are not parsed by the env path — pass the raw byte count.

---

## A minimal config

The smallest config that will start the bot in bot mode with aria2 and Telegram leech:

```json
{
  "telegram": {
    "api_id": 1234567,
    "api_hash": "0123456789abcdef0123456789abcdef",
    "bot_token": "123456789:AAH...",
    "owner_id": 987654321
  },
  "aria2": {
    "rpc_url": "ws://127.0.0.1:6800/jsonrpc",
    "rpc_secret": "REPLACE_ME"
  },
  "paths": {
    "downloads": "/var/lib/cmlb/downloads",
    "data": "/var/lib/cmlb/data"
  }
}
```

---

## A fully-populated config

Every section, every field, defaults shown explicitly. Useful as a starting template.

```json
{
  "telegram": {
    "api_id": 1234567,
    "api_hash": "0123456789abcdef0123456789abcdef",
    "bot_token": "123456789:AAH...",
    "owner_id": 987654321,
    "admins": [111111111],
    "users": [222222222, 333333333],
    "allowed_chats": [],
    "tdlib_directory": "/var/lib/cmlb/data/tdlib",
    "tdlib_log_verbosity": 1,
    "tdlib_use_test_dc": false,
    "progress_edit_interval_ms": 2000,
    "upload_split_bytes": 2000000000,
    "parse_mode": "html"
  },
  "aria2": {
    "enabled": true,
    "rpc_url": "ws://127.0.0.1:6800/jsonrpc",
    "rpc_secret": "REPLACE_ME",
    "connect_timeout_ms": 10000,
    "request_timeout_ms": 30000,
    "max_concurrent_downloads": 4,
    "max_connection_per_server": 8,
    "split": 8,
    "min_split_size": "10M",
    "seed_time_minutes": 0,
    "seed_ratio": 1.0,
    "bt_tracker": []
  },
  "qbittorrent": {
    "enabled": false,
    "base_url": "http://127.0.0.1:8080",
    "username": "admin",
    "password": "REPLACE_ME",
    "category": "cmlb",
    "save_path": "/var/lib/cmlb/downloads",
    "seed_time_minutes": 0,
    "seed_ratio": -1.0,
    "request_timeout_ms": 30000,
    "verify_tls": true
  },
  "rclone": {
    "enabled": false,
    "binary": "rclone",
    "config_path": "/var/lib/cmlb/data/rclone.conf",
    "default_remote": "",
    "extra_args": ["--transfers", "4"],
    "verify_tls": true
  },
  "google_drive": {
    "enabled": false,
    "service_account_path": "/var/lib/cmlb/data/service_account.json",
    "default_folder_id": "",
    "shared_drive_id": "",
    "use_resumable_upload_threshold_bytes": 5000000,
    "chunk_size_bytes": 8388608,
    "request_timeout_ms": 60000,
    "index_url": ""
  },
  "database": {
    "path": "/var/lib/cmlb/data/cmlb.db",
    "connection_pool_size": 4,
    "busy_timeout_ms": 5000,
    "journal_mode": "wal",
    "synchronous": "normal",
    "foreign_keys": true,
    "migrate_on_start": true
  },
  "logging": {
    "level": "info",
    "format": "text",
    "file": "/var/lib/cmlb/data/cmlb.log",
    "file_max_size_bytes": 10485760,
    "file_max_files": 5,
    "console": true,
    "redact_secrets": true,
    "module_levels": {}
  },
  "paths": {
    "downloads": "/var/lib/cmlb/downloads",
    "data": "/var/lib/cmlb/data",
    "tmp": "/tmp",
    "keep_downloads_on_success": false,
    "keep_downloads_on_failure": true
  },
  "executor": {
    "worker_threads": 8,
    "blocking_pool_threads": 4,
    "cancellation_grace_ms": 5000
  },
  "metrics": {
    "enabled": false,
    "bind": "127.0.0.1:9464",
    "path": "/metrics"
  },
  "rss": {
    "enabled": true,
    "default_interval_seconds": 600,
    "min_interval_seconds": 60,
    "user_agent": "CMLB/0.1 (+https://github.com/staneswilson/cpp-mirror-leech-bot)",
    "request_timeout_ms": 30000,
    "max_entries_per_poll": 25
  },
  "limits": {
    "max_concurrent_tasks_per_user": 2,
    "max_concurrent_tasks_global": 16,
    "max_filesize_bytes": 0,
    "daily_quota_bytes_per_user": 0,
    "command_cooldown_ms": 1000
  }
}
```
