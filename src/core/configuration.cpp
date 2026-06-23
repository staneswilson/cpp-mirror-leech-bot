#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::core {

namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// JSON helpers — explicit type checks, default-on-missing where appropriate.
// ---------------------------------------------------------------------------

template <typename T>
void read_field(const json& obj, std::string_view key, T& out, std::vector<AppError>& errors) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return; // leave `out` at its default
    }
    try {
        if constexpr (std::is_same_v<T, std::filesystem::path>) {
            out = std::filesystem::path{it->get<std::string>()};
        } else {
            out = it->get<T>();
        }
    } catch (const json::exception& ex) {
        errors.emplace_back(ErrorCode::JsonParse,
                            std::string{"field '"} + std::string{key} + "': " + ex.what());
    }
}

void read_seconds(const json& obj,
                  std::string_view key,
                  std::chrono::seconds& out,
                  std::vector<AppError>& errors) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return;
    try {
        out = std::chrono::seconds{it->get<std::int64_t>()};
    } catch (const json::exception& ex) {
        errors.emplace_back(ErrorCode::JsonParse,
                            std::string{"field '"} + std::string{key} + "': " + ex.what());
    }
}

void read_minutes(const json& obj,
                  std::string_view key,
                  std::chrono::minutes& out,
                  std::vector<AppError>& errors) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return;
    try {
        out = std::chrono::minutes{it->get<std::int64_t>()};
    } catch (const json::exception& ex) {
        errors.emplace_back(ErrorCode::JsonParse,
                            std::string{"field '"} + std::string{key} + "': " + ex.what());
    }
}

void read_millis(const json& obj,
                 std::string_view key,
                 std::chrono::milliseconds& out,
                 std::vector<AppError>& errors) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return;
    try {
        out = std::chrono::milliseconds{it->get<std::int64_t>()};
    } catch (const json::exception& ex) {
        errors.emplace_back(ErrorCode::JsonParse,
                            std::string{"field '"} + std::string{key} + "': " + ex.what());
    }
}

const json* section(const json& doc, std::string_view key) noexcept {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_object())
        return nullptr;
    return &*it;
}

// ---------------------------------------------------------------------------
// Env-var helpers — non-throwing parsing.
// ---------------------------------------------------------------------------

std::optional<std::string> env(const char* name) noexcept {
#if defined(_MSC_VER)
    // MSVC deprecates std::getenv (C4996). _dupenv_s is the recommended
    // portable-on-Windows replacement; it allocates a buffer the caller frees.
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
        return std::nullopt;
    }
    std::string value{buf};
    std::free(buf);
    return value;
#else
    if (const char* v = std::getenv(name); v != nullptr) {
        return std::string{v};
    }
    return std::nullopt;
#endif
}

template <typename Int>
std::optional<Int> parse_int(std::string_view text) noexcept {
    Int value{};
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last)
        return std::nullopt;
    return value;
}

std::optional<double> parse_double(std::string_view text) noexcept {
    // std::from_chars for double is not yet portable across all stdlibs we
    // target — fall back to strtod with explicit error handling.
    std::string buf{text};
    char* end_ptr = nullptr;
    errno = 0;
    const double value = std::strtod(buf.c_str(), &end_ptr);
    if (errno != 0 || end_ptr == buf.c_str() || end_ptr != buf.c_str() + buf.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parse_bool(std::string_view text) noexcept {
    if (text == "1" || text == "true" || text == "TRUE" || text == "True" || text == "yes"
        || text == "on")
        return true;
    if (text == "0" || text == "false" || text == "FALSE" || text == "False" || text == "no"
        || text == "off")
        return false;
    return std::nullopt;
}

// Log a one-shot warning when an env override fails to parse. Used in
// apply_env_overrides so operators don't lose their intended setting silently.
// noexcept-friendly: Logger::warn does not throw under normal conditions, and
// if it ever did the calling function's noexcept marker would correctly trip
// std::terminate — same blast radius as any other allocation OOM.
void warn_bad_env(std::string_view name,
                  std::string_view value,
                  std::string_view expected) noexcept {
    cmlb::core::Logger::warn("configuration: ignoring malformed env override {}='{}' (expected {})",
                             name,
                             value,
                             expected);
}

// Typed env-override helpers. Each looks up the env var, applies it on success,
// and logs a warn on failure. Returning void keeps the call sites concise:
//   env_int("CMLB_ARIA2_SPLIT", cfg.aria2.split);
template <typename Int>
void env_int(const char* name, Int& out) noexcept {
    static_assert(std::is_integral_v<Int>, "env_int requires integral target");
    if (auto v = env(name); v) {
        if (auto p = parse_int<Int>(*v))
            out = *p;
        else
            warn_bad_env(name, *v, "integer");
    }
}

void env_bool(const char* name, bool& out) noexcept {
    if (auto v = env(name); v) {
        if (auto p = parse_bool(*v))
            out = *p;
        else
            warn_bad_env(name, *v, "bool (1/0/true/false/yes/no/on/off)");
    }
}

void env_double(const char* name, double& out) noexcept {
    if (auto v = env(name); v) {
        if (auto p = parse_double(*v))
            out = *p;
        else
            warn_bad_env(name, *v, "decimal");
    }
}

void env_seconds(const char* name, std::chrono::seconds& out) noexcept {
    if (auto v = env(name); v) {
        if (auto p = parse_int<std::int64_t>(*v))
            out = std::chrono::seconds{*p};
        else
            warn_bad_env(name, *v, "integer seconds");
    }
}

void env_minutes(const char* name, std::chrono::minutes& out) noexcept {
    if (auto v = env(name); v) {
        if (auto p = parse_int<std::int64_t>(*v))
            out = std::chrono::minutes{*p};
        else
            warn_bad_env(name, *v, "integer minutes");
    }
}

void env_millis(const char* name, std::chrono::milliseconds& out) noexcept {
    if (auto v = env(name); v) {
        if (auto p = parse_int<std::int64_t>(*v))
            out = std::chrono::milliseconds{*p};
        else
            warn_bad_env(name, *v, "integer milliseconds");
    }
}

std::vector<std::int64_t> parse_id_list(std::string_view text) noexcept {
    std::vector<std::int64_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        // Skip separators (`,` and whitespace).
        while (i < text.size() && (text[i] == ',' || text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && text[i] != ',' && text[i] != ' ' && text[i] != '\t') {
            ++i;
        }
        if (start == i)
            break;
        if (auto v = parse_int<std::int64_t>(text.substr(start, i - start)); v) {
            out.push_back(*v);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Section parsers.
// ---------------------------------------------------------------------------

void parse_telegram(const json& obj, TelegramConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "api_id", out.api_id, errors);
    read_field(obj, "api_hash", out.api_hash, errors);
    read_field(obj, "bot_token", out.bot_token, errors);
    read_field(obj, "database_directory", out.database_directory, errors);
    read_field(obj, "owner_id", out.owner_id, errors);
    read_field(obj, "sudo_users", out.sudo_users, errors);
    read_field(obj, "authorized_chats", out.authorized_chats, errors);
    read_field(obj, "upload_chunk_size_kb", out.upload_chunk_size_kb, errors);
    read_field(obj, "download_chunk_size_kb", out.download_chunk_size_kb, errors);
    read_field(obj, "connection_retry_count_max", out.connection_retry_count_max, errors);
    read_field(obj, "prefer_ipv6", out.prefer_ipv6, errors);
    read_field(obj, "upload_parallelism", out.upload_parallelism, errors);
    read_field(obj, "upload_files_parallelism", out.upload_files_parallelism, errors);
}

void parse_aria2(const json& obj, Aria2Config& out, std::vector<AppError>& errors) {
    read_field(obj, "rpc_url", out.rpc_url, errors);
    read_field(obj, "secret", out.secret, errors);
    read_field(obj, "max_concurrent_downloads", out.max_concurrent_downloads, errors);
    read_seconds(obj, "request_timeout", out.request_timeout, errors);
    read_field(obj, "max_connection_per_server", out.max_connection_per_server, errors);
    read_field(obj, "split", out.split, errors);
    read_field(obj, "min_split_size", out.min_split_size, errors);
    read_field(obj, "disk_cache", out.disk_cache, errors);
    read_field(obj, "max_tries", out.max_tries, errors);
    read_seconds(obj, "retry_wait", out.retry_wait, errors);
    read_field(obj, "max_overall_download_limit", out.max_overall_download_limit, errors);
    read_field(obj, "max_overall_upload_limit", out.max_overall_upload_limit, errors);
    read_field(obj, "enable_dht", out.enable_dht, errors);
    read_field(obj, "enable_pex", out.enable_pex, errors);
    read_field(obj, "bt_max_peers", out.bt_max_peers, errors);
    read_field(obj, "user_agent", out.user_agent, errors);
}

void parse_qbittorrent(const json& obj, QbittorrentConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "url", out.url, errors);
    read_field(obj, "username", out.username, errors);
    read_field(obj, "password", out.password, errors);
    read_field(obj, "seed_ratio_limit", out.seed_ratio_limit, errors);
    read_minutes(obj, "seed_time_limit", out.seed_time_limit, errors);
    read_field(obj, "max_active_downloads", out.max_active_downloads, errors);
    read_field(obj, "max_active_uploads", out.max_active_uploads, errors);
    read_field(obj, "max_active_torrents", out.max_active_torrents, errors);
    read_field(obj, "max_connections", out.max_connections, errors);
    read_field(obj, "max_connections_per_torrent", out.max_connections_per_torrent, errors);
    read_field(obj, "max_uploads", out.max_uploads, errors);
    read_field(obj, "max_uploads_per_torrent", out.max_uploads_per_torrent, errors);
    read_field(obj, "up_limit", out.up_limit, errors);
    read_field(obj, "dl_limit", out.dl_limit, errors);
    read_field(obj, "dht", out.dht, errors);
    read_field(obj, "pex", out.pex, errors);
    read_field(obj, "lsd", out.lsd, errors);
    read_field(obj, "anonymous_mode", out.anonymous_mode, errors);
    read_field(obj, "async_io_threads", out.async_io_threads, errors);
    read_field(obj, "disk_cache_mib", out.disk_cache_mib, errors);
}

void parse_rclone(const json& obj, RcloneConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "executable", out.executable, errors);
    // Tolerate the legacy "path" alias.
    if (!obj.contains("executable") && obj.contains("path")) {
        read_field(obj, "path", out.executable, errors);
    }
    read_field(obj, "config_path", out.config_path, errors);
    read_field(obj, "transfers", out.transfers, errors);
    read_field(obj, "checkers", out.checkers, errors);
    read_field(obj, "multi_thread_streams", out.multi_thread_streams, errors);
    read_field(obj, "multi_thread_cutoff", out.multi_thread_cutoff, errors);
    read_field(obj, "drive_chunk_size", out.drive_chunk_size, errors);
    read_field(obj, "buffer_size", out.buffer_size, errors);
    read_field(obj, "use_mmap", out.use_mmap, errors);
    read_field(obj, "fast_list", out.fast_list, errors);
    read_field(obj, "drive_acknowledge_abuse", out.drive_acknowledge_abuse, errors);
    read_field(obj, "log_level", out.log_level, errors);
    read_field(obj, "extra_args", out.extra_args, errors);
}

void parse_gdrive(const json& obj, GoogleDriveConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "credentials_path", out.credentials_path, errors);
    read_field(obj, "parent_folder_id", out.parent_folder_id, errors);
    read_field(obj, "use_service_accounts", out.use_service_accounts, errors);
    read_field(obj, "sa_folder", out.sa_folder, errors);
    read_field(obj, "chunk_size", out.chunk_size, errors);
    read_field(obj, "parallel_chunks_per_file", out.parallel_chunks_per_file, errors);
    read_field(obj, "parallel_files_per_directory", out.parallel_files_per_directory, errors);
    read_field(obj, "max_retries", out.max_retries, errors);
    read_millis(obj, "initial_retry_delay", out.initial_retry_delay, errors);
}

void parse_database(const json& obj, DatabaseConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "path", out.path, errors);
    read_millis(obj, "busy_timeout", out.busy_timeout, errors);
    read_field(obj, "wal_mode", out.wal_mode, errors);
}

void parse_logging(const json& obj, LoggingConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "logs_dir", out.logs_dir, errors);
    read_field(obj, "level", out.level, errors);
    read_field(obj, "console", out.console, errors);
}

void parse_paths(const json& obj, PathsConfig& out, std::vector<AppError>& errors) {
    read_field(obj, "download_dir", out.download_dir, errors);
    read_field(obj, "data_dir", out.data_dir, errors);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<AppConfig> Configuration::load(const std::filesystem::path& path) {
    std::ifstream stream{path};
    if (!stream) {
        return error(ErrorCode::FileSystem, "cannot open config file: " + path.string());
    }
    json doc;
    try {
        doc = json::parse(stream, nullptr, false);
    } catch (const std::exception& ex) {
        return error(ErrorCode::Io,
                     std::string{"failed to read "} + path.string() + ": " + ex.what());
    }
    if (doc.is_discarded()) {
        return error(ErrorCode::JsonParse, "JSON parse error in " + path.string());
    }
    return from_json(doc);
}

Result<AppConfig> Configuration::from_json(const json& doc) {
    if (!doc.is_object()) {
        return error(ErrorCode::InvalidConfiguration, "root of config JSON must be an object");
    }

    AppConfig cfg{};
    std::vector<AppError> parse_errors;

    if (const auto* s = section(doc, "telegram"))
        parse_telegram(*s, cfg.telegram, parse_errors);
    if (const auto* s = section(doc, "aria2"))
        parse_aria2(*s, cfg.aria2, parse_errors);
    if (const auto* s = section(doc, "qbittorrent"))
        parse_qbittorrent(*s, cfg.qbittorrent, parse_errors);
    if (const auto* s = section(doc, "rclone"))
        parse_rclone(*s, cfg.rclone, parse_errors);

    // Accept both "google_drive" (preferred) and legacy "gdrive".
    if (const auto* s = section(doc, "google_drive")) {
        parse_gdrive(*s, cfg.google_drive, parse_errors);
    } else if (const auto* s_legacy = section(doc, "gdrive")) {
        parse_gdrive(*s_legacy, cfg.google_drive, parse_errors);
    }

    if (const auto* s = section(doc, "database"))
        parse_database(*s, cfg.database, parse_errors);
    if (const auto* s = section(doc, "logging"))
        parse_logging(*s, cfg.logging, parse_errors);
    if (const auto* s = section(doc, "paths"))
        parse_paths(*s, cfg.paths, parse_errors);

    // Top-level legacy fields kept for backward compatibility with the
    // existing config.example.json.
    {
        std::vector<AppError> legacy_errors;
        std::filesystem::path legacy_dl;
        read_field(doc, "download_dir", legacy_dl, legacy_errors);
        if (!legacy_dl.empty())
            cfg.paths.download_dir = legacy_dl;

        std::string legacy_level;
        read_field(doc, "log_level", legacy_level, legacy_errors);
        if (!legacy_level.empty())
            cfg.logging.level = legacy_level;
        // Suppress legacy parse errors — these fields are optional aliases.
    }

    apply_env_overrides(cfg);

    auto validation = validate(cfg);
    parse_errors.insert(parse_errors.end(),
                        std::make_move_iterator(validation.begin()),
                        std::make_move_iterator(validation.end()));

    if (!parse_errors.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < parse_errors.size(); ++i) {
            if (i != 0)
                joined.push_back('\n');
            joined += '[';
            joined += error_code_name(parse_errors[i].code);
            joined += "] ";
            joined += parse_errors[i].message;
        }
        return error(ErrorCode::InvalidConfiguration, std::move(joined));
    }

    return cfg;
}

std::vector<AppError> Configuration::validate(const AppConfig& cfg) {
    std::vector<AppError> errors;

    // Telegram
    if (cfg.telegram.api_id == 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "telegram.api_id must be non-zero");
    }
    if (cfg.telegram.api_hash.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "telegram.api_hash must not be empty");
    }
    if (cfg.telegram.owner_id == 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "telegram.owner_id must be non-zero");
    }
    if (cfg.telegram.bot_token.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.bot_token must not be empty");
    }
    if (cfg.telegram.upload_chunk_size_kb < 1 || cfg.telegram.upload_chunk_size_kb > 65536) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.upload_chunk_size_kb must be in [1, 65536]");
    }
    if (cfg.telegram.download_chunk_size_kb < 1 || cfg.telegram.download_chunk_size_kb > 65536) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.download_chunk_size_kb must be in [1, 65536]");
    }
    if (cfg.telegram.connection_retry_count_max < 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.connection_retry_count_max must be >= 0");
    }
    if (cfg.telegram.upload_parallelism < 1 || cfg.telegram.upload_parallelism > 32) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.upload_parallelism must be in [1, 32]");
    }
    if (cfg.telegram.upload_files_parallelism < 1 || cfg.telegram.upload_files_parallelism > 32) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "telegram.upload_files_parallelism must be in [1, 32]");
    }

    // aria2
    if (cfg.aria2.rpc_url.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "aria2.rpc_url must not be empty");
    } else if (!cfg.aria2.rpc_url.starts_with("ws://")
               && !cfg.aria2.rpc_url.starts_with("wss://")) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.rpc_url must start with ws:// or wss://");
    }
    if (cfg.aria2.max_concurrent_downloads <= 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.max_concurrent_downloads must be positive");
    }
    if (cfg.aria2.request_timeout <= std::chrono::seconds{0}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.request_timeout must be positive");
    }
    if (cfg.aria2.max_connection_per_server < 1 || cfg.aria2.max_connection_per_server > 16) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.max_connection_per_server must be in [1, 16]");
    }
    if (cfg.aria2.split < 1 || cfg.aria2.split > 16) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "aria2.split must be in [1, 16]");
    }
    if (cfg.aria2.max_tries < 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "aria2.max_tries must be >= 0");
    }
    if (cfg.aria2.retry_wait < std::chrono::seconds{0}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "aria2.retry_wait must be >= 0");
    }
    if (cfg.aria2.bt_max_peers < 1) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "aria2.bt_max_peers must be >= 1");
    }
    if (cfg.aria2.max_overall_download_limit < 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.max_overall_download_limit must be >= 0");
    }
    if (cfg.aria2.max_overall_upload_limit < 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "aria2.max_overall_upload_limit must be >= 0");
    }

    // qBittorrent
    if (cfg.qbittorrent.url.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "qbittorrent.url must not be empty");
    } else if (!cfg.qbittorrent.url.starts_with("http://")
               && !cfg.qbittorrent.url.starts_with("https://")) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.url must start with http:// or https:// "
                            "(got '"
                                + cfg.qbittorrent.url + "')");
    }
    if (cfg.qbittorrent.seed_ratio_limit < 0.0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.seed_ratio_limit must be >= 0");
    }
    if (cfg.qbittorrent.seed_time_limit < std::chrono::minutes{0}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.seed_time_limit must be >= 0");
    }
    auto qbit_pos = [&](const char* name, int v) {
        if (v < 1) {
            errors.emplace_back(ErrorCode::InvalidConfiguration,
                                std::string{"qbittorrent."} + name + " must be >= 1");
        }
    };
    qbit_pos("max_active_downloads", cfg.qbittorrent.max_active_downloads);
    qbit_pos("max_active_uploads", cfg.qbittorrent.max_active_uploads);
    qbit_pos("max_active_torrents", cfg.qbittorrent.max_active_torrents);
    qbit_pos("max_connections", cfg.qbittorrent.max_connections);
    qbit_pos("max_connections_per_torrent", cfg.qbittorrent.max_connections_per_torrent);
    qbit_pos("max_uploads", cfg.qbittorrent.max_uploads);
    qbit_pos("max_uploads_per_torrent", cfg.qbittorrent.max_uploads_per_torrent);
    qbit_pos("async_io_threads", cfg.qbittorrent.async_io_threads);
    if (cfg.qbittorrent.disk_cache_mib < -1) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.disk_cache_mib must be -1 or >= 0");
    }
    // qBit treats up_limit / dl_limit as bytes/sec. -1 means unlimited; 0 means
    // "leave whatever qBit had configured." Anything else negative is a bug.
    if (cfg.qbittorrent.up_limit < -1) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.up_limit must be -1 (unlimited), "
                            "0 (qBit default), or > 0 (bytes/sec)");
    }
    if (cfg.qbittorrent.dl_limit < -1) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "qbittorrent.dl_limit must be -1 (unlimited), "
                            "0 (qBit default), or > 0 (bytes/sec)");
    }

    // rclone — upper bounds keep operators from typing 9999 and DoS-ing
    // themselves with file-descriptor exhaustion or RAM thrashing.
    if (cfg.rclone.transfers < 1 || cfg.rclone.transfers > 64) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "rclone.transfers must be in [1, 64]");
    }
    if (cfg.rclone.checkers < 1 || cfg.rclone.checkers > 256) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "rclone.checkers must be in [1, 256]");
    }
    if (cfg.rclone.multi_thread_streams < 1 || cfg.rclone.multi_thread_streams > 64) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "rclone.multi_thread_streams must be in [1, 64]");
    }
    {
        const auto& lvl = cfg.rclone.log_level;
        if (lvl != "DEBUG" && lvl != "INFO" && lvl != "NOTICE" && lvl != "ERROR") {
            errors.emplace_back(ErrorCode::InvalidConfiguration,
                                "rclone.log_level must be one of "
                                "DEBUG, INFO, NOTICE, ERROR (got '"
                                    + lvl + "')");
        }
    }

    // Google Drive
    constexpr std::size_t kMinChunk = 256 * 1024;                                  // 256 KiB
    constexpr std::size_t kMaxChunk = static_cast<std::size_t>(512) * 1024 * 1024; // 512 MiB
    if (cfg.google_drive.chunk_size < kMinChunk || cfg.google_drive.chunk_size > kMaxChunk) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.chunk_size must be in [256 KiB, 512 MiB]");
    }
    if (cfg.google_drive.chunk_size % kMinChunk != 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.chunk_size must be a multiple of 256 KiB");
    }
    if (cfg.google_drive.parallel_chunks_per_file < 1
        || cfg.google_drive.parallel_chunks_per_file > 64) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.parallel_chunks_per_file must be in [1, 64]");
    }
    if (cfg.google_drive.parallel_files_per_directory < 1
        || cfg.google_drive.parallel_files_per_directory > 64) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.parallel_files_per_directory must be in [1, 64]");
    }
    if (cfg.google_drive.max_retries < 0) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.max_retries must be >= 0");
    }
    if (cfg.google_drive.initial_retry_delay < std::chrono::milliseconds{0}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "google_drive.initial_retry_delay must be >= 0");
    }

    // Database
    if (cfg.database.path.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "database.path must not be empty");
    }
    // SQLite busy_timeout is honoured as milliseconds; capping at 60 s keeps
    // a misconfigured value from hanging every repository call indefinitely
    // (an unindexed table lock would otherwise stall the whole bot).
    if (cfg.database.busy_timeout < std::chrono::milliseconds{0}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "database.busy_timeout must be >= 0");
    } else if (cfg.database.busy_timeout > std::chrono::milliseconds{60'000}) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "database.busy_timeout must be <= 60000 ms "
                            "(longer waits indicate a deeper deadlock; raise "
                            "deliberately if absolutely required)");
    }

    // Logging
    if (!parse_log_level(cfg.logging.level)) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "logging.level is not a recognized level: " + cfg.logging.level);
    }
    if (cfg.logging.logs_dir.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "logging.logs_dir must not be empty");
    }

    // Paths
    if (cfg.paths.download_dir.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration,
                            "paths.download_dir must not be empty");
    }
    if (cfg.paths.data_dir.empty()) {
        errors.emplace_back(ErrorCode::InvalidConfiguration, "paths.data_dir must not be empty");
    }

    return errors;
}

void Configuration::apply_env_overrides(AppConfig& cfg) noexcept {
    // ---- Telegram ----
    env_int("CMLB_TELEGRAM_API_ID", cfg.telegram.api_id);
    if (auto v = env("CMLB_TELEGRAM_API_HASH"))
        cfg.telegram.api_hash = std::move(*v);
    if (auto v = env("CMLB_TELEGRAM_BOT_TOKEN"))
        cfg.telegram.bot_token = std::move(*v);
    if (auto v = env("CMLB_TELEGRAM_DATABASE_DIRECTORY"))
        cfg.telegram.database_directory = std::filesystem::path{*v};
    env_int("CMLB_TELEGRAM_OWNER_ID", cfg.telegram.owner_id);
    if (auto v = env("CMLB_TELEGRAM_SUDO_USERS"))
        cfg.telegram.sudo_users = parse_id_list(*v);
    if (auto v = env("CMLB_TELEGRAM_AUTHORIZED_CHATS"))
        cfg.telegram.authorized_chats = parse_id_list(*v);
    env_int("CMLB_TELEGRAM_UPLOAD_CHUNK_SIZE_KB", cfg.telegram.upload_chunk_size_kb);
    env_int("CMLB_TELEGRAM_DOWNLOAD_CHUNK_SIZE_KB", cfg.telegram.download_chunk_size_kb);
    env_int("CMLB_TELEGRAM_CONNECTION_RETRY_COUNT_MAX", cfg.telegram.connection_retry_count_max);
    env_bool("CMLB_TELEGRAM_PREFER_IPV6", cfg.telegram.prefer_ipv6);
    env_int("CMLB_TELEGRAM_UPLOAD_PARALLELISM", cfg.telegram.upload_parallelism);
    env_int("CMLB_TELEGRAM_UPLOAD_FILES_PARALLELISM", cfg.telegram.upload_files_parallelism);

    // ---- aria2 ----
    if (auto v = env("CMLB_ARIA2_RPC_URL"))
        cfg.aria2.rpc_url = std::move(*v);
    if (auto v = env("CMLB_ARIA2_SECRET"))
        cfg.aria2.secret = std::move(*v);
    env_int("CMLB_ARIA2_MAX_CONCURRENT_DOWNLOADS", cfg.aria2.max_concurrent_downloads);
    env_seconds("CMLB_ARIA2_REQUEST_TIMEOUT", cfg.aria2.request_timeout);
    env_int("CMLB_ARIA2_MAX_CONNECTION_PER_SERVER", cfg.aria2.max_connection_per_server);
    env_int("CMLB_ARIA2_SPLIT", cfg.aria2.split);
    if (auto v = env("CMLB_ARIA2_MIN_SPLIT_SIZE"))
        cfg.aria2.min_split_size = std::move(*v);
    if (auto v = env("CMLB_ARIA2_DISK_CACHE"))
        cfg.aria2.disk_cache = std::move(*v);
    env_int("CMLB_ARIA2_MAX_TRIES", cfg.aria2.max_tries);
    env_seconds("CMLB_ARIA2_RETRY_WAIT", cfg.aria2.retry_wait);
    env_int("CMLB_ARIA2_MAX_OVERALL_DOWNLOAD_LIMIT", cfg.aria2.max_overall_download_limit);
    env_int("CMLB_ARIA2_MAX_OVERALL_UPLOAD_LIMIT", cfg.aria2.max_overall_upload_limit);
    env_bool("CMLB_ARIA2_ENABLE_DHT", cfg.aria2.enable_dht);
    env_bool("CMLB_ARIA2_ENABLE_PEX", cfg.aria2.enable_pex);
    env_int("CMLB_ARIA2_BT_MAX_PEERS", cfg.aria2.bt_max_peers);
    if (auto v = env("CMLB_ARIA2_USER_AGENT"))
        cfg.aria2.user_agent = std::move(*v);

    // ---- qBittorrent ----
    if (auto v = env("CMLB_QBITTORRENT_URL"))
        cfg.qbittorrent.url = std::move(*v);
    if (auto v = env("CMLB_QBITTORRENT_USERNAME"))
        cfg.qbittorrent.username = std::move(*v);
    if (auto v = env("CMLB_QBITTORRENT_PASSWORD"))
        cfg.qbittorrent.password = std::move(*v);
    env_double("CMLB_QBITTORRENT_SEED_RATIO_LIMIT", cfg.qbittorrent.seed_ratio_limit);
    env_minutes("CMLB_QBITTORRENT_SEED_TIME_LIMIT", cfg.qbittorrent.seed_time_limit);
    env_int("CMLB_QBITTORRENT_MAX_ACTIVE_DOWNLOADS", cfg.qbittorrent.max_active_downloads);
    env_int("CMLB_QBITTORRENT_MAX_ACTIVE_UPLOADS", cfg.qbittorrent.max_active_uploads);
    env_int("CMLB_QBITTORRENT_MAX_ACTIVE_TORRENTS", cfg.qbittorrent.max_active_torrents);
    env_int("CMLB_QBITTORRENT_MAX_CONNECTIONS", cfg.qbittorrent.max_connections);
    env_int("CMLB_QBITTORRENT_MAX_CONNECTIONS_PER_TORRENT",
            cfg.qbittorrent.max_connections_per_torrent);
    env_int("CMLB_QBITTORRENT_MAX_UPLOADS", cfg.qbittorrent.max_uploads);
    env_int("CMLB_QBITTORRENT_MAX_UPLOADS_PER_TORRENT", cfg.qbittorrent.max_uploads_per_torrent);
    env_int("CMLB_QBITTORRENT_UP_LIMIT", cfg.qbittorrent.up_limit);
    env_int("CMLB_QBITTORRENT_DL_LIMIT", cfg.qbittorrent.dl_limit);
    env_bool("CMLB_QBITTORRENT_DHT", cfg.qbittorrent.dht);
    env_bool("CMLB_QBITTORRENT_PEX", cfg.qbittorrent.pex);
    env_bool("CMLB_QBITTORRENT_LSD", cfg.qbittorrent.lsd);
    env_bool("CMLB_QBITTORRENT_ANONYMOUS_MODE", cfg.qbittorrent.anonymous_mode);
    env_int("CMLB_QBITTORRENT_ASYNC_IO_THREADS", cfg.qbittorrent.async_io_threads);
    env_int("CMLB_QBITTORRENT_DISK_CACHE_MIB", cfg.qbittorrent.disk_cache_mib);

    // ---- rclone ----
    if (auto v = env("CMLB_RCLONE_EXECUTABLE"))
        cfg.rclone.executable = std::filesystem::path{*v};
    if (auto v = env("CMLB_RCLONE_CONFIG_PATH"))
        cfg.rclone.config_path = std::filesystem::path{*v};
    env_int("CMLB_RCLONE_TRANSFERS", cfg.rclone.transfers);
    env_int("CMLB_RCLONE_CHECKERS", cfg.rclone.checkers);
    env_int("CMLB_RCLONE_MULTI_THREAD_STREAMS", cfg.rclone.multi_thread_streams);
    if (auto v = env("CMLB_RCLONE_MULTI_THREAD_CUTOFF"))
        cfg.rclone.multi_thread_cutoff = std::move(*v);
    if (auto v = env("CMLB_RCLONE_DRIVE_CHUNK_SIZE"))
        cfg.rclone.drive_chunk_size = std::move(*v);
    if (auto v = env("CMLB_RCLONE_BUFFER_SIZE"))
        cfg.rclone.buffer_size = std::move(*v);
    env_bool("CMLB_RCLONE_USE_MMAP", cfg.rclone.use_mmap);
    env_bool("CMLB_RCLONE_FAST_LIST", cfg.rclone.fast_list);
    env_bool("CMLB_RCLONE_DRIVE_ACKNOWLEDGE_ABUSE", cfg.rclone.drive_acknowledge_abuse);
    if (auto v = env("CMLB_RCLONE_LOG_LEVEL"))
        cfg.rclone.log_level = std::move(*v);

    // ---- Google Drive ----
    if (auto v = env("CMLB_GOOGLE_DRIVE_CREDENTIALS_PATH"))
        cfg.google_drive.credentials_path = std::filesystem::path{*v};
    if (auto v = env("CMLB_GOOGLE_DRIVE_PARENT_FOLDER_ID"))
        cfg.google_drive.parent_folder_id = std::move(*v);
    env_bool("CMLB_GOOGLE_DRIVE_USE_SERVICE_ACCOUNTS", cfg.google_drive.use_service_accounts);
    if (auto v = env("CMLB_GOOGLE_DRIVE_SA_FOLDER"))
        cfg.google_drive.sa_folder = std::filesystem::path{*v};
    env_int("CMLB_GOOGLE_DRIVE_CHUNK_SIZE", cfg.google_drive.chunk_size);
    env_int("CMLB_GOOGLE_DRIVE_PARALLEL_CHUNKS_PER_FILE",
            cfg.google_drive.parallel_chunks_per_file);
    env_int("CMLB_GOOGLE_DRIVE_PARALLEL_FILES_PER_DIRECTORY",
            cfg.google_drive.parallel_files_per_directory);
    env_int("CMLB_GOOGLE_DRIVE_MAX_RETRIES", cfg.google_drive.max_retries);
    env_millis("CMLB_GOOGLE_DRIVE_INITIAL_RETRY_DELAY_MS", cfg.google_drive.initial_retry_delay);

    // ---- Database ----
    if (auto v = env("CMLB_DATABASE_PATH"))
        cfg.database.path = std::filesystem::path{*v};
    env_millis("CMLB_DATABASE_BUSY_TIMEOUT", cfg.database.busy_timeout);
    env_bool("CMLB_DATABASE_WAL_MODE", cfg.database.wal_mode);

    // ---- Logging ----
    if (auto v = env("CMLB_LOGGING_LOGS_DIR"))
        cfg.logging.logs_dir = std::filesystem::path{*v};
    if (auto v = env("CMLB_LOGGING_LEVEL"))
        cfg.logging.level = std::move(*v);
    env_bool("CMLB_LOGGING_CONSOLE", cfg.logging.console);

    // ---- Paths ----
    if (auto v = env("CMLB_PATHS_DOWNLOAD_DIR"))
        cfg.paths.download_dir = std::filesystem::path{*v};
    if (auto v = env("CMLB_PATHS_DATA_DIR"))
        cfg.paths.data_dir = std::filesystem::path{*v};
}

} // namespace cmlb::core
