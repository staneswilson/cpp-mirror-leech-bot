#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <cmlb/core/error.hpp>

namespace cmlb::core {

/// Telegram / TDLib client configuration.
struct TelegramConfig {
    std::int32_t api_id{0};
    std::string  api_hash;
    std::string  bot_token;
    std::filesystem::path database_directory{"tdlib"};
    std::int64_t owner_id{0};
    std::vector<std::int64_t> sudo_users;
    std::vector<std::int64_t> authorized_chats;
    /// TDLib `upload_chunk_size_kb` option. Larger chunks = fewer round-trips
    /// per file, higher throughput. TDLib clamps server-side.
    int  upload_chunk_size_kb{2048};
    /// TDLib `download_chunk_size_kb` option.
    int  download_chunk_size_kb{1024};
    /// TDLib `connection_retry_count_max` option. Number of reconnect attempts
    /// before giving up on a stale connection.
    int  connection_retry_count_max{5};
    /// Prefer IPv6 endpoints when both are available.
    bool prefer_ipv6{false};
    /// Number of file parts kept in-flight in `TelegramUploader::upload_file`
    /// when a file is large enough to be split. Each part is a separate
    /// `messages.sendMedia` over TDLib.
    int  upload_parallelism{4};

    /// Number of distinct files kept in-flight by
    /// `TelegramUploader::upload_directory`. TDLib pipelines its own upload
    /// sessions, so multiple concurrent `sendMedia` calls saturate the link
    /// more fully than a strict per-file loop. Defaults to a conservative `2`;
    /// raise on directories with many small files.
    int  upload_files_parallelism{2};
};

/// aria2 JSON-RPC client configuration.
///
/// The throughput-related fields (everything past `request_timeout`) are
/// passed to aria2's `aria2.changeGlobalOption` / `aria2.addUri` so the
/// daemon uses them for every task we start. They are NOT enforced by the
/// bot — aria2 itself clamps to its compiled-in maxima (e.g. `split` ≤ 16).
struct Aria2Config {
    std::string rpc_url{"ws://localhost:6800/jsonrpc"};
    std::string secret;
    int         max_concurrent_downloads{5};
    std::chrono::seconds request_timeout{30};
    /// aria2 `max-connection-per-server`. Hard upper bound 16.
    int          max_connection_per_server{16};
    /// aria2 `split`. Hard upper bound 16.
    int          split{16};
    /// aria2 `min-split-size`. Uses aria2 size syntax (`1M`, `512K`...).
    std::string  min_split_size{"1M"};
    /// aria2 `disk-cache`. Larger reduces SSD write amplification.
    std::string  disk_cache{"128M"};
    /// aria2 `max-tries`. Per-URL retry count.
    int          max_tries{5};
    /// aria2 `retry-wait`. Wait between retries.
    std::chrono::seconds retry_wait{5};
    /// aria2 `max-overall-download-limit`. 0 = unlimited.
    std::int64_t max_overall_download_limit{0};
    /// aria2 `max-overall-upload-limit`. 0 = unlimited.
    std::int64_t max_overall_upload_limit{0};
    /// BitTorrent: enable DHT.
    bool         enable_dht{true};
    /// BitTorrent: enable PeerEXchange.
    bool         enable_pex{true};
    /// BitTorrent: maximum peers per torrent.
    int          bt_max_peers{55};
    /// User-Agent reported on HTTP(S) downloads.
    std::string  user_agent{"aria2/1.37.0"};
};

/// qBittorrent Web API configuration.
///
/// The post-login tunables are pushed to `/api/v2/app/setPreferences` on
/// every successful auth. qBit silently ignores unknown keys on older
/// versions; failures are logged at `warn` (not fatal).
struct QbittorrentConfig {
    std::string url{"http://localhost:8080"};
    std::string username{"admin"};
    std::string password;
    double      seed_ratio_limit{1.0};
    std::chrono::minutes seed_time_limit{60};
    /// `max_active_downloads` preference.
    int max_active_downloads{8};
    /// `max_active_uploads` preference.
    int max_active_uploads{8};
    /// `max_active_torrents` preference.
    int max_active_torrents{16};
    /// `max_connec` (global connection cap).
    int max_connections{500};
    /// `max_connec_per_torrent`.
    int max_connections_per_torrent{100};
    /// `max_uploads` (global slots).
    int max_uploads{20};
    /// `max_uploads_per_torrent`.
    int max_uploads_per_torrent{5};
    /// `up_limit` bytes/sec. -1 = unlimited; 0 = leave qBit default.
    std::int64_t up_limit{0};
    /// `dl_limit` bytes/sec. -1 = unlimited; 0 = leave qBit default.
    std::int64_t dl_limit{0};
    /// `dht`, `pex`, `lsd` BitTorrent discovery preferences.
    bool dht{true};
    bool pex{true};
    bool lsd{true};
    /// `anonymous_mode` preference.
    bool anonymous_mode{false};
    /// `async_io_threads` — qBit-specific I/O worker pool size.
    int async_io_threads{8};
    /// `disk_cache` MiB. -1 lets qBit auto-size.
    int disk_cache_mib{256};
};

/// rclone binary configuration (invoked as a subprocess).
///
/// Each tunable maps directly to an rclone CLI flag. Defaults aim for
/// "fast on a gigabit link with reasonable RAM" — tune down for low-RAM
/// hosts (especially `buffer_size` × `transfers` is the main RAM cost).
struct RcloneConfig {
    std::filesystem::path executable{"rclone"};
    std::filesystem::path config_path;
    /// `--transfers`. Number of parallel file transfers.
    int          transfers{8};
    /// `--checkers`. Parallelism for hash/size checks.
    int          checkers{16};
    /// `--multi-thread-streams`. Streams per large file.
    int          multi_thread_streams{4};
    /// `--multi-thread-cutoff`. Threshold above which multi-thread kicks in.
    std::string  multi_thread_cutoff{"250M"};
    /// `--drive-chunk-size`. GDrive remote chunk; clamped to 256K alignment.
    std::string  drive_chunk_size{"64M"};
    /// `--buffer-size`. Per-transfer read-ahead buffer. RAM cost = N × this.
    std::string  buffer_size{"32M"};
    /// `--use-mmap`. mmap the read buffer (lower RSS, faster on large files).
    bool         use_mmap{true};
    /// `--fast-list`. Single API listing per directory tree (saves round-trips).
    bool         fast_list{true};
    /// `--drive-acknowledge-abuse`. Allow downloads flagged as abusive.
    bool         drive_acknowledge_abuse{true};
    /// `--log-level`. One of `DEBUG`, `INFO`, `NOTICE`, `ERROR`.
    std::string  log_level{"NOTICE"};
    /// Escape hatch — additional flags appended verbatim to every invocation.
    std::vector<std::string> extra_args;
};

/// Google Drive (mirror) configuration.
struct GoogleDriveConfig {
    std::filesystem::path credentials_path{"service_account.json"};
    std::string parent_folder_id;
    bool use_service_accounts{true};
    std::filesystem::path sa_folder{"accounts"};
    std::size_t chunk_size{8 * 1024 * 1024};  // 8 MiB
    /// Number of concurrent chunk PUTs against a single resumable session.
    /// GDrive accepts out-of-order ranges, so fan-out scales near-linearly
    /// until the link saturates.
    int parallel_chunks_per_file{4};
    /// Number of files uploaded in parallel in `upload_directory`.
    int parallel_files_per_directory{4};
    /// Max retry attempts on 429 / 5xx with exponential backoff.
    int max_retries{6};
    /// Initial backoff before first retry; doubles each attempt.
    std::chrono::milliseconds initial_retry_delay{500};
};

/// SQLite database configuration.
struct DatabaseConfig {
    std::filesystem::path path{"data/cmlb.db"};
    std::chrono::milliseconds busy_timeout{5000};
    bool wal_mode{true};
};

/// Logging subsystem configuration.
struct LoggingConfig {
    std::filesystem::path logs_dir{"logs"};
    std::string level{"info"};
    bool console{true};
};

/// Runtime directory layout configuration.
struct PathsConfig {
    std::filesystem::path download_dir{"downloads"};
    std::filesystem::path data_dir{"data"};
};

/// Aggregate of every config section. The single source of truth for
/// runtime-tunable settings.
struct AppConfig {
    TelegramConfig    telegram;
    Aria2Config       aria2;
    QbittorrentConfig qbittorrent;
    RcloneConfig      rclone;
    GoogleDriveConfig google_drive;
    DatabaseConfig    database;
    LoggingConfig     logging;
    PathsConfig       paths;
};

/// Configuration loader / validator.
///
/// Responsibilities:
///  * Read a JSON file from disk.
///  * Coerce values into strongly-typed structs (`AppConfig`).
///  * Apply environment-variable overrides (`CMLB_*`).
///  * Run cross-field validation and return *all* errors at once.
class Configuration {
public:
    Configuration() = delete;

    /// Loads, env-overrides, and validates a config file. Returns the
    /// populated `AppConfig` on success or an `AppError` whose `message`
    /// concatenates every validation failure (one per line).
    [[nodiscard]] static Result<AppConfig> load(const std::filesystem::path& path);

    /// Parses an already-loaded JSON document. Performs env-overrides and
    /// validation. Same error semantics as `load`.
    [[nodiscard]] static Result<AppConfig> from_json(const nlohmann::json& doc);

    /// Returns every validation error in `cfg`. Empty vector means valid.
    [[nodiscard]] static std::vector<AppError> validate(const AppConfig& cfg);

    /// Applies `CMLB_*` environment overrides in-place. Silently ignores
    /// missing vars; on malformed numeric overrides, the value is left
    /// untouched (the subsequent validation pass will surface invalid state).
    static void apply_env_overrides(AppConfig& cfg) noexcept;
};

}  // namespace cmlb::core
