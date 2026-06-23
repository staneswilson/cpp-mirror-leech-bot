# CMLB Configuration Reference

This document describes the `config.json` schema currently accepted by
`cmlb::core::Configuration`. It intentionally mirrors
[`config.example.json`](../config.example.json) and
[`include/cmlb/core/configuration.hpp`](../include/cmlb/core/configuration.hpp);
do not document future fields here until the loader reads them.

Configuration is parsed once at startup. Runtime reload is not supported in
v1; change the file or environment, then restart the service.

---

## Loading Order

1. The CLI resolves the config path from the optional positional `CONFIG_PATH`
   argument, defaulting to `./config.json` when no path is supplied.
2. `Configuration::load` parses JSON into strongly typed config structs.
3. `CMLB_*` environment overrides are applied.
4. Validation runs and returns every error in one report.
5. The immutable `AppConfig` is dependency-injected into the composition root.

Use this before deploy:

```bash
cmlb --validate-config /etc/cmlb/config.json
```

---

## Top-Level Sections

```json
{
  "telegram": {},
  "aria2": {},
  "qbittorrent": {},
  "rclone": {},
  "google_drive": {},
  "database": {},
  "logging": {},
  "paths": {}
}
```

Unknown top-level sections are ignored by the current loader. Avoid adding
operator-facing fields until they are implemented and tested.

---

## `telegram`

Required. TDLib identity, authorization, and Telegram throughput tuning.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `api_id` | int32 | `0` | Required, must be non-zero. |
| `api_hash` | string | `""` | Required. Secret. |
| `bot_token` | string | `""` | Required in v1. Secret. |
| `database_directory` | path | `"tdlib"` | TDLib session directory. Losing it forces re-auth. |
| `owner_id` | int64 | `0` | Required, must be non-zero. |
| `sudo_users` | int64 array | `[]` | Admin-tier user IDs. |
| `authorized_chats` | int64 array | `[]` | If non-empty, commands outside these chats are ignored. |
| `upload_chunk_size_kb` | int | `2048` | `[1, 65536]`; sent to TDLib `upload_chunk_size_kb`. |
| `download_chunk_size_kb` | int | `1024` | `[1, 65536]`; sent to TDLib `download_chunk_size_kb`. |
| `connection_retry_count_max` | int | `5` | `>= 0`; sent to TDLib. |
| `prefer_ipv6` | bool | `false` | Sent to TDLib when supported. |
| `upload_parallelism` | int | `4` | `[1, 32]`; split-file Telegram upload workers. |
| `upload_files_parallelism` | int | `2` | `[1, 32]`; directory file upload workers. |

---

## `aria2`

aria2 JSON-RPC downloader configuration. CMLB uses aria2 for direct URLs and
can use it for torrent/magnet work.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `rpc_url` | string | `"ws://localhost:6800/jsonrpc"` | Must start with `ws://` or `wss://`. |
| `secret` | string | `""` | Optional RPC token; secret. |
| `max_concurrent_downloads` | int | `5` | Must be positive. |
| `request_timeout` | seconds | `30` | Must be positive. |
| `max_connection_per_server` | int | `16` | `[1, 16]`; aria2 upstream cap is 16. |
| `split` | int | `16` | `[1, 16]`; aria2 upstream cap is 16. |
| `min_split_size` | string | `"1M"` | aria2 size syntax. |
| `disk_cache` | string | `"128M"` | aria2 size syntax. |
| `max_tries` | int | `5` | `>= 0`. |
| `retry_wait` | seconds | `5` | `>= 0`. |
| `max_overall_download_limit` | int64 | `0` | Bytes/sec; `0` means unlimited. |
| `max_overall_upload_limit` | int64 | `0` | Bytes/sec; `0` means unlimited. |
| `enable_dht` | bool | `true` | BitTorrent DHT. |
| `enable_pex` | bool | `true` | BitTorrent peer exchange. |
| `bt_max_peers` | int | `55` | Must be `>= 1`. |
| `user_agent` | string | `"aria2/1.37.0"` | HTTP(S) User-Agent for aria2. |

---

## `qbittorrent`

qBittorrent Web API downloader configuration.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `url` | string | `"http://localhost:8080"` | Must start with `http://` or `https://`. |
| `username` | string | `"admin"` | Web UI username. |
| `password` | string | `""` | Web UI password; secret. |
| `seed_ratio_limit` | double | `1.0` | `>= 0`; pushed to qBit preferences. |
| `seed_time_limit` | minutes | `60` | `>= 0`; pushed to qBit preferences. |
| `max_active_downloads` | int | `8` | `>= 1`. |
| `max_active_uploads` | int | `8` | `>= 1`. |
| `max_active_torrents` | int | `16` | `>= 1`. |
| `max_connections` | int | `500` | `>= 1`. |
| `max_connections_per_torrent` | int | `100` | `>= 1`. |
| `max_uploads` | int | `20` | `>= 1`. |
| `max_uploads_per_torrent` | int | `5` | `>= 1`. |
| `up_limit` | int64 | `0` | `-1` unlimited; `0` leaves qBit default; positive = bytes/sec. |
| `dl_limit` | int64 | `0` | `-1` unlimited; `0` leaves qBit default; positive = bytes/sec. |
| `dht` | bool | `true` | Pushed to qBit preferences. |
| `pex` | bool | `true` | Pushed to qBit preferences. |
| `lsd` | bool | `true` | Pushed to qBit preferences. |
| `anonymous_mode` | bool | `false` | Pushed to qBit preferences. |
| `async_io_threads` | int | `8` | `>= 1`. |
| `disk_cache_mib` | int | `256` | `-1` lets qBit auto-size; otherwise `>= 0`. |

---

## `rclone`

rclone uploader configuration. These fields map directly to `rclone copy`
flags.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `executable` | path | `"rclone"` | Binary path or PATH-resolved command. Legacy key `path` is also accepted. |
| `config_path` | path | empty | Optional rclone configuration file path. |
| `transfers` | int | `8` | `[1, 64]`; RAM cost scales with `buffer_size`. |
| `checkers` | int | `16` | `[1, 256]`. |
| `multi_thread_streams` | int | `4` | `[1, 64]`. |
| `multi_thread_cutoff` | string | `"250M"` | rclone size syntax. |
| `drive_chunk_size` | string | `"64M"` | rclone size syntax. |
| `buffer_size` | string | `"32M"` | Per-transfer buffer. |
| `use_mmap` | bool | `true` | Adds `--use-mmap`. |
| `fast_list` | bool | `true` | Adds `--fast-list`. |
| `drive_acknowledge_abuse` | bool | `true` | Adds Drive abuse acknowledgement flag. |
| `log_level` | string | `"NOTICE"` | One of `DEBUG`, `INFO`, `NOTICE`, `ERROR`. |
| `extra_args` | string array | `[]` | Appended verbatim; no environment override. |

---

## `google_drive`

Google Drive direct uploader and clone/count/delete support.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `credentials_path` | path | `"service_account.json"` | Service-account JSON key path; secret-bearing file. |
| `parent_folder_id` | string | `""` | Destination folder ID. |
| `use_service_accounts` | bool | `true` | Uses service-account credentials in v1. |
| `sa_folder` | path | `"accounts"` | Directory for service-account material. |
| `chunk_size` | size_t | `8388608` | `[256 KiB, 512 MiB]`, multiple of 256 KiB. |
| `parallel_chunks_per_file` | int | `4` | `[1, 64]`; resumable upload PUT workers. |
| `parallel_files_per_directory` | int | `4` | `[1, 64]`; directory upload workers. |
| `max_retries` | int | `6` | `>= 0`; retry attempts on 429/5xx. |
| `initial_retry_delay` | milliseconds | `500` | `>= 0`; doubles between retries. |

---

## `database`

SQLite persistence configuration.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `path` | path | `"data/cmlb.db"` | Database file path. |
| `busy_timeout` | milliseconds | `5000` | `[0, 60000]`; caps write lock waits. |
| `wal_mode` | bool | `true` | Enables SQLite WAL mode. |

Startup migrations are idempotent and forward-only.

---

## `logging`

spdlog async logger configuration.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `logs_dir` | path | `"logs"` | Created if missing. Rotating file is `logs_dir/cmlb.log`. |
| `level` | string | `"info"` | `trace`, `debug`, `info`, `warn`, `warning`, `error`, `err`, `critical`, `fatal`, or `off`. |
| `console` | bool | `true` | Adds colored stderr sink. |

Rotation is fixed in code at 10 MiB per file and 5 retained files.

---

## `paths`

Runtime directory layout.

| Field | Type | Default | Validation / notes |
|---|---:|---:|---|
| `download_dir` | path | `"downloads"` | Working directory for downloaded files. |
| `data_dir` | path | `"data"` | Long-lived state directory. |

---

## Environment Overrides

Environment overrides are explicit. These are the currently supported names:

Set an override as a normal environment variable before starting `cmlb`:

```bash
export CMLB_TELEGRAM_API_ID=123456
export CMLB_TELEGRAM_OWNER_ID=987654321
cmlb /etc/cmlb/config.json
```

For Docker Compose, put deployment values in `packaging/.env`; the compose file
maps them to the `CMLB_*` variables inside the container. For systemd, use an
override file:

```bash
sudo systemctl edit cmlb
# add under [Service]:
# Environment=CMLB_LOGGING_LEVEL=debug
sudo systemctl restart cmlb
```

| JSON path | Environment variable |
|---|---|
| `telegram.api_id` | `CMLB_TELEGRAM_API_ID` |
| `telegram.api_hash` | `CMLB_TELEGRAM_API_HASH` |
| `telegram.bot_token` | `CMLB_TELEGRAM_BOT_TOKEN` |
| `telegram.database_directory` | `CMLB_TELEGRAM_DATABASE_DIRECTORY` |
| `telegram.owner_id` | `CMLB_TELEGRAM_OWNER_ID` |
| `telegram.sudo_users` | `CMLB_TELEGRAM_SUDO_USERS` |
| `telegram.authorized_chats` | `CMLB_TELEGRAM_AUTHORIZED_CHATS` |
| `telegram.upload_chunk_size_kb` | `CMLB_TELEGRAM_UPLOAD_CHUNK_SIZE_KB` |
| `telegram.download_chunk_size_kb` | `CMLB_TELEGRAM_DOWNLOAD_CHUNK_SIZE_KB` |
| `telegram.connection_retry_count_max` | `CMLB_TELEGRAM_CONNECTION_RETRY_COUNT_MAX` |
| `telegram.prefer_ipv6` | `CMLB_TELEGRAM_PREFER_IPV6` |
| `telegram.upload_parallelism` | `CMLB_TELEGRAM_UPLOAD_PARALLELISM` |
| `telegram.upload_files_parallelism` | `CMLB_TELEGRAM_UPLOAD_FILES_PARALLELISM` |
| `aria2.rpc_url` | `CMLB_ARIA2_RPC_URL` |
| `aria2.secret` | `CMLB_ARIA2_SECRET` |
| `aria2.max_concurrent_downloads` | `CMLB_ARIA2_MAX_CONCURRENT_DOWNLOADS` |
| `aria2.request_timeout` | `CMLB_ARIA2_REQUEST_TIMEOUT` |
| `aria2.max_connection_per_server` | `CMLB_ARIA2_MAX_CONNECTION_PER_SERVER` |
| `aria2.split` | `CMLB_ARIA2_SPLIT` |
| `aria2.min_split_size` | `CMLB_ARIA2_MIN_SPLIT_SIZE` |
| `aria2.disk_cache` | `CMLB_ARIA2_DISK_CACHE` |
| `aria2.max_tries` | `CMLB_ARIA2_MAX_TRIES` |
| `aria2.retry_wait` | `CMLB_ARIA2_RETRY_WAIT` |
| `aria2.max_overall_download_limit` | `CMLB_ARIA2_MAX_OVERALL_DOWNLOAD_LIMIT` |
| `aria2.max_overall_upload_limit` | `CMLB_ARIA2_MAX_OVERALL_UPLOAD_LIMIT` |
| `aria2.enable_dht` | `CMLB_ARIA2_ENABLE_DHT` |
| `aria2.enable_pex` | `CMLB_ARIA2_ENABLE_PEX` |
| `aria2.bt_max_peers` | `CMLB_ARIA2_BT_MAX_PEERS` |
| `aria2.user_agent` | `CMLB_ARIA2_USER_AGENT` |
| `qbittorrent.url` | `CMLB_QBITTORRENT_URL` |
| `qbittorrent.username` | `CMLB_QBITTORRENT_USERNAME` |
| `qbittorrent.password` | `CMLB_QBITTORRENT_PASSWORD` |
| `qbittorrent.seed_ratio_limit` | `CMLB_QBITTORRENT_SEED_RATIO_LIMIT` |
| `qbittorrent.seed_time_limit` | `CMLB_QBITTORRENT_SEED_TIME_LIMIT` |
| `qbittorrent.max_active_downloads` | `CMLB_QBITTORRENT_MAX_ACTIVE_DOWNLOADS` |
| `qbittorrent.max_active_uploads` | `CMLB_QBITTORRENT_MAX_ACTIVE_UPLOADS` |
| `qbittorrent.max_active_torrents` | `CMLB_QBITTORRENT_MAX_ACTIVE_TORRENTS` |
| `qbittorrent.max_connections` | `CMLB_QBITTORRENT_MAX_CONNECTIONS` |
| `qbittorrent.max_connections_per_torrent` | `CMLB_QBITTORRENT_MAX_CONNECTIONS_PER_TORRENT` |
| `qbittorrent.max_uploads` | `CMLB_QBITTORRENT_MAX_UPLOADS` |
| `qbittorrent.max_uploads_per_torrent` | `CMLB_QBITTORRENT_MAX_UPLOADS_PER_TORRENT` |
| `qbittorrent.up_limit` | `CMLB_QBITTORRENT_UP_LIMIT` |
| `qbittorrent.dl_limit` | `CMLB_QBITTORRENT_DL_LIMIT` |
| `qbittorrent.dht` | `CMLB_QBITTORRENT_DHT` |
| `qbittorrent.pex` | `CMLB_QBITTORRENT_PEX` |
| `qbittorrent.lsd` | `CMLB_QBITTORRENT_LSD` |
| `qbittorrent.anonymous_mode` | `CMLB_QBITTORRENT_ANONYMOUS_MODE` |
| `qbittorrent.async_io_threads` | `CMLB_QBITTORRENT_ASYNC_IO_THREADS` |
| `qbittorrent.disk_cache_mib` | `CMLB_QBITTORRENT_DISK_CACHE_MIB` |
| `rclone.executable` | `CMLB_RCLONE_EXECUTABLE` |
| `rclone.config_path` | `CMLB_RCLONE_CONFIG_PATH` |
| `rclone.transfers` | `CMLB_RCLONE_TRANSFERS` |
| `rclone.checkers` | `CMLB_RCLONE_CHECKERS` |
| `rclone.multi_thread_streams` | `CMLB_RCLONE_MULTI_THREAD_STREAMS` |
| `rclone.multi_thread_cutoff` | `CMLB_RCLONE_MULTI_THREAD_CUTOFF` |
| `rclone.drive_chunk_size` | `CMLB_RCLONE_DRIVE_CHUNK_SIZE` |
| `rclone.buffer_size` | `CMLB_RCLONE_BUFFER_SIZE` |
| `rclone.use_mmap` | `CMLB_RCLONE_USE_MMAP` |
| `rclone.fast_list` | `CMLB_RCLONE_FAST_LIST` |
| `rclone.drive_acknowledge_abuse` | `CMLB_RCLONE_DRIVE_ACKNOWLEDGE_ABUSE` |
| `rclone.log_level` | `CMLB_RCLONE_LOG_LEVEL` |
| `google_drive.credentials_path` | `CMLB_GOOGLE_DRIVE_CREDENTIALS_PATH` |
| `google_drive.parent_folder_id` | `CMLB_GOOGLE_DRIVE_PARENT_FOLDER_ID` |
| `google_drive.use_service_accounts` | `CMLB_GOOGLE_DRIVE_USE_SERVICE_ACCOUNTS` |
| `google_drive.sa_folder` | `CMLB_GOOGLE_DRIVE_SA_FOLDER` |
| `google_drive.chunk_size` | `CMLB_GOOGLE_DRIVE_CHUNK_SIZE` |
| `google_drive.parallel_chunks_per_file` | `CMLB_GOOGLE_DRIVE_PARALLEL_CHUNKS_PER_FILE` |
| `google_drive.parallel_files_per_directory` | `CMLB_GOOGLE_DRIVE_PARALLEL_FILES_PER_DIRECTORY` |
| `google_drive.max_retries` | `CMLB_GOOGLE_DRIVE_MAX_RETRIES` |
| `google_drive.initial_retry_delay` | `CMLB_GOOGLE_DRIVE_INITIAL_RETRY_DELAY_MS` |
| `database.path` | `CMLB_DATABASE_PATH` |
| `database.busy_timeout` | `CMLB_DATABASE_BUSY_TIMEOUT` |
| `database.wal_mode` | `CMLB_DATABASE_WAL_MODE` |
| `logging.logs_dir` | `CMLB_LOGGING_LOGS_DIR` |
| `logging.level` | `CMLB_LOGGING_LEVEL` |
| `logging.console` | `CMLB_LOGGING_CONSOLE` |
| `paths.download_dir` | `CMLB_PATHS_DOWNLOAD_DIR` |
| `paths.data_dir` | `CMLB_PATHS_DATA_DIR` |

Boolean environment overrides accept `1/0`, `true/false` in lower, upper, or
title case, plus lower-case `yes/no` and `on/off`.
Comma-separated ID lists are accepted for `sudo_users` and `authorized_chats`.
Malformed override values are logged as warnings and ignored, then normal
validation runs on the resulting config.

---

## Minimal Config

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
    "secret": "REPLACE_ME"
  },
  "paths": {
    "download_dir": "/var/lib/cmlb/downloads",
    "data_dir": "/var/lib/cmlb"
  },
  "logging": {
    "logs_dir": "/var/log/cmlb",
    "level": "info",
    "console": true
  }
}
```

For a full template with every current field, use
[`config.example.json`](../config.example.json).
