#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>

using cmlb::core::AppConfig;
using cmlb::core::Configuration;
using cmlb::core::ErrorCode;
using Catch::Matchers::ContainsSubstring;

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

namespace {

// Cross-platform env helpers (Windows lacks setenv/unsetenv).
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/// RAII wrapper that sets an env var on construction and unsets on destruction.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value)
        : name_{std::move(name)} {
        set_env(name_.c_str(), value.c_str());
    }
    ~ScopedEnv() { unset_env(name_.c_str()); }
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&)                 = delete;
    ScopedEnv& operator=(ScopedEnv&&)      = delete;
private:
    std::string name_;
};

/// RAII tempdir.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64 gen{rd()};
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("cmlb_cfg_" + std::to_string(gen()));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error{"could not create temp directory"};
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&)                 = delete;
    TempDir& operator=(TempDir&&)      = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

nlohmann::json valid_doc() {
    return nlohmann::json{
        {"telegram", {
            {"api_id", 12345},
            {"api_hash", "deadbeefcafebabedeadbeefcafebabe"},
            {"bot_token", "123:abc"},
            {"database_directory", "tdlib"},
            {"owner_id", 9001},
            {"sudo_users", nlohmann::json::array({1, 2, 3})},
            {"authorized_chats", nlohmann::json::array()},
        }},
        {"aria2", {
            {"rpc_url", "ws://localhost:6800/jsonrpc"},
            {"secret", "topsecret"},
            {"max_concurrent_downloads", 4},
            {"request_timeout", 20},
        }},
        {"qbittorrent", {
            {"url", "http://localhost:8080"},
            {"username", "admin"},
            {"password", "pw"},
            {"seed_ratio_limit", 2.5},
            {"seed_time_limit", 30},
        }},
        {"rclone", {{"executable", "rclone"}, {"config_path", ""}}},
        {"google_drive", {
            {"credentials_path", "creds.json"},
            {"parent_folder_id", "root"},
            {"use_service_accounts", false},
            {"sa_folder", "accounts"},
            {"chunk_size", 4 * 1024 * 1024},
        }},
        {"database", {
            {"path", "data/cmlb.db"},
            {"busy_timeout", 5000},
            {"wal_mode", true},
        }},
        {"logging", {
            {"logs_dir", "logs"},
            {"level", "info"},
            {"console", true},
        }},
        {"paths", {
            {"download_dir", "downloads"},
            {"data_dir", "data"},
        }},
    };
}

// Ensure env-var noise from a previous run / shell doesn't bleed in.
void clear_cmlb_env() {
    static const char* const kVars[] = {
        "CMLB_TELEGRAM_API_ID", "CMLB_TELEGRAM_API_HASH", "CMLB_TELEGRAM_BOT_TOKEN",
        "CMLB_TELEGRAM_DATABASE_DIRECTORY", "CMLB_TELEGRAM_OWNER_ID",
        "CMLB_TELEGRAM_SUDO_USERS", "CMLB_TELEGRAM_AUTHORIZED_CHATS",
        "CMLB_TELEGRAM_UPLOAD_CHUNK_SIZE_KB", "CMLB_TELEGRAM_DOWNLOAD_CHUNK_SIZE_KB",
        "CMLB_TELEGRAM_CONNECTION_RETRY_COUNT_MAX", "CMLB_TELEGRAM_PREFER_IPV6",
        "CMLB_TELEGRAM_UPLOAD_PARALLELISM", "CMLB_TELEGRAM_UPLOAD_FILES_PARALLELISM",
        "CMLB_ARIA2_RPC_URL", "CMLB_ARIA2_SECRET",
        "CMLB_ARIA2_MAX_CONCURRENT_DOWNLOADS", "CMLB_ARIA2_REQUEST_TIMEOUT",
        "CMLB_ARIA2_MAX_CONNECTION_PER_SERVER", "CMLB_ARIA2_SPLIT",
        "CMLB_ARIA2_MIN_SPLIT_SIZE", "CMLB_ARIA2_DISK_CACHE",
        "CMLB_ARIA2_MAX_TRIES", "CMLB_ARIA2_RETRY_WAIT",
        "CMLB_ARIA2_MAX_OVERALL_DOWNLOAD_LIMIT", "CMLB_ARIA2_MAX_OVERALL_UPLOAD_LIMIT",
        "CMLB_ARIA2_ENABLE_DHT", "CMLB_ARIA2_ENABLE_PEX",
        "CMLB_ARIA2_BT_MAX_PEERS", "CMLB_ARIA2_USER_AGENT",
        "CMLB_QBITTORRENT_URL", "CMLB_QBITTORRENT_USERNAME",
        "CMLB_QBITTORRENT_PASSWORD", "CMLB_QBITTORRENT_SEED_RATIO_LIMIT",
        "CMLB_QBITTORRENT_SEED_TIME_LIMIT",
        "CMLB_QBITTORRENT_MAX_ACTIVE_DOWNLOADS", "CMLB_QBITTORRENT_MAX_ACTIVE_UPLOADS",
        "CMLB_QBITTORRENT_MAX_ACTIVE_TORRENTS", "CMLB_QBITTORRENT_MAX_CONNECTIONS",
        "CMLB_QBITTORRENT_MAX_CONNECTIONS_PER_TORRENT",
        "CMLB_QBITTORRENT_MAX_UPLOADS", "CMLB_QBITTORRENT_MAX_UPLOADS_PER_TORRENT",
        "CMLB_QBITTORRENT_UP_LIMIT", "CMLB_QBITTORRENT_DL_LIMIT",
        "CMLB_QBITTORRENT_DHT", "CMLB_QBITTORRENT_PEX", "CMLB_QBITTORRENT_LSD",
        "CMLB_QBITTORRENT_ANONYMOUS_MODE", "CMLB_QBITTORRENT_ASYNC_IO_THREADS",
        "CMLB_QBITTORRENT_DISK_CACHE_MIB",
        "CMLB_RCLONE_EXECUTABLE", "CMLB_RCLONE_CONFIG_PATH",
        "CMLB_RCLONE_TRANSFERS", "CMLB_RCLONE_CHECKERS",
        "CMLB_RCLONE_MULTI_THREAD_STREAMS", "CMLB_RCLONE_MULTI_THREAD_CUTOFF",
        "CMLB_RCLONE_DRIVE_CHUNK_SIZE", "CMLB_RCLONE_BUFFER_SIZE",
        "CMLB_RCLONE_USE_MMAP", "CMLB_RCLONE_FAST_LIST",
        "CMLB_RCLONE_DRIVE_ACKNOWLEDGE_ABUSE", "CMLB_RCLONE_LOG_LEVEL",
        "CMLB_GOOGLE_DRIVE_CREDENTIALS_PATH", "CMLB_GOOGLE_DRIVE_PARENT_FOLDER_ID",
        "CMLB_GOOGLE_DRIVE_USE_SERVICE_ACCOUNTS", "CMLB_GOOGLE_DRIVE_SA_FOLDER",
        "CMLB_GOOGLE_DRIVE_CHUNK_SIZE",
        "CMLB_GOOGLE_DRIVE_PARALLEL_CHUNKS_PER_FILE",
        "CMLB_GOOGLE_DRIVE_PARALLEL_FILES_PER_DIRECTORY",
        "CMLB_GOOGLE_DRIVE_MAX_RETRIES", "CMLB_GOOGLE_DRIVE_INITIAL_RETRY_DELAY_MS",
        "CMLB_DATABASE_PATH", "CMLB_DATABASE_BUSY_TIMEOUT", "CMLB_DATABASE_WAL_MODE",
        "CMLB_LOGGING_LOGS_DIR", "CMLB_LOGGING_LEVEL", "CMLB_LOGGING_CONSOLE",
        "CMLB_PATHS_DOWNLOAD_DIR", "CMLB_PATHS_DATA_DIR",
    };
    for (const auto* v : kVars) unset_env(v);
}

}  // namespace

TEST_CASE("Configuration::from_json with a valid document yields populated AppConfig",
          "[core][configuration]") {
    clear_cmlb_env();
    auto result = Configuration::from_json(valid_doc());
    REQUIRE(result.has_value());

    const AppConfig& cfg = *result;
    CHECK(cfg.telegram.api_id == 12345);
    CHECK(cfg.telegram.api_hash == "deadbeefcafebabedeadbeefcafebabe");
    CHECK(cfg.telegram.owner_id == 9001);
    CHECK(cfg.telegram.sudo_users.size() == 3);
    CHECK(cfg.aria2.rpc_url == "ws://localhost:6800/jsonrpc");
    CHECK(cfg.aria2.max_concurrent_downloads == 4);
    CHECK(cfg.qbittorrent.seed_ratio_limit == 2.5);
    CHECK(cfg.google_drive.chunk_size == static_cast<std::size_t>(4 * 1024 * 1024));
    CHECK(cfg.database.wal_mode == true);
    CHECK(cfg.logging.level == "info");
    CHECK(cfg.paths.download_dir == std::filesystem::path{"downloads"});
}

TEST_CASE("Configuration::from_json rejects missing/zero required fields with InvalidConfiguration",
          "[core][configuration]") {
    clear_cmlb_env();

    SECTION("api_id == 0") {
        auto doc = valid_doc();
        doc["telegram"]["api_id"] = 0;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidConfiguration);
        CHECK_THAT(result.error().message, ContainsSubstring("api_id"));
    }

    SECTION("empty api_hash") {
        auto doc = valid_doc();
        doc["telegram"]["api_hash"] = "";
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidConfiguration);
        CHECK_THAT(result.error().message, ContainsSubstring("api_hash"));
    }

    SECTION("bad aria2.rpc_url scheme") {
        auto doc = valid_doc();
        doc["aria2"]["rpc_url"] = "http://localhost";
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidConfiguration);
        CHECK_THAT(result.error().message, ContainsSubstring("ws://"));
    }

    SECTION("chunk_size out of range") {
        auto doc = valid_doc();
        doc["google_drive"]["chunk_size"] = 16;  // too small
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidConfiguration);
        CHECK_THAT(result.error().message, ContainsSubstring("chunk_size"));
    }

    SECTION("multiple errors are concatenated") {
        auto doc = valid_doc();
        doc["telegram"]["api_id"] = 0;
        doc["telegram"]["owner_id"] = 0;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("api_id"));
        CHECK_THAT(result.error().message, ContainsSubstring("owner_id"));
    }
}

TEST_CASE("Configuration::load reads a file and reports filesystem errors",
          "[core][configuration]") {
    clear_cmlb_env();

    SECTION("missing file") {
        auto result = Configuration::load("/non/existent/path/does/not/exist.json");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::FileSystem);
    }

    SECTION("malformed JSON") {
        TempDir tmp;
        const auto path = tmp.path() / "bad.json";
        {
            std::ofstream f{path};
            f << "{ this is not json";
        }
        auto result = Configuration::load(path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::JsonParse);
    }

    SECTION("valid file round-trip") {
        TempDir tmp;
        const auto path = tmp.path() / "ok.json";
        {
            std::ofstream f{path};
            f << valid_doc().dump();
        }
        auto result = Configuration::load(path);
        REQUIRE(result.has_value());
        CHECK(result->telegram.api_id == 12345);
    }
}

TEST_CASE("Env-var overrides apply, then unset cleanly", "[core][configuration]") {
    clear_cmlb_env();
    {
        ScopedEnv api_id{"CMLB_TELEGRAM_API_ID", "777"};
        ScopedEnv hash{"CMLB_TELEGRAM_API_HASH", "envhash"};
        ScopedEnv rpc{"CMLB_ARIA2_RPC_URL", "wss://custom:1234/x"};
        ScopedEnv chunk{"CMLB_GOOGLE_DRIVE_CHUNK_SIZE", "8388608"};  // 8 MiB
        ScopedEnv ratio{"CMLB_QBITTORRENT_SEED_RATIO_LIMIT", "3.25"};
        ScopedEnv wal{"CMLB_DATABASE_WAL_MODE", "false"};

        auto result = Configuration::from_json(valid_doc());
        REQUIRE(result.has_value());
        CHECK(result->telegram.api_id == 777);
        CHECK(result->telegram.api_hash == "envhash");
        CHECK(result->aria2.rpc_url == "wss://custom:1234/x");
        CHECK(result->google_drive.chunk_size == static_cast<std::size_t>(8 * 1024 * 1024));
        CHECK(result->qbittorrent.seed_ratio_limit == 3.25);
        CHECK(result->database.wal_mode == false);
    }
    // After scope, env vars are cleared - second load uses JSON values.
    auto result = Configuration::from_json(valid_doc());
    REQUIRE(result.has_value());
    CHECK(result->telegram.api_id == 12345);
    CHECK(result->aria2.rpc_url == "ws://localhost:6800/jsonrpc");
}

TEST_CASE("Configuration::validate is exhaustive", "[core][configuration]") {
    clear_cmlb_env();
    AppConfig cfg{};  // all defaults - many required fields are unset
    const auto errors = Configuration::validate(cfg);
    // api_id, api_hash, bot_token, owner_id all unset → at least 4 errors.
    CHECK(errors.size() >= 4);
}

// ---------------------------------------------------------------------------
// Throughput tunables — added by the hyper-speed pass. The point of these
// cases is twofold: (1) lock the defaults so a future "tweak the defaults"
// PR cannot silently regress operator-facing behavior, and (2) prove that
// every CMLB_* env-var override reaches the right struct field.
// ---------------------------------------------------------------------------

TEST_CASE("Throughput defaults are applied when JSON omits the fields",
          "[core][configuration][throughput]") {
    clear_cmlb_env();
    auto result = Configuration::from_json(valid_doc());
    REQUIRE(result.has_value());
    const AppConfig& cfg = *result;

    // Telegram.
    CHECK(cfg.telegram.upload_chunk_size_kb      == 2048);
    CHECK(cfg.telegram.download_chunk_size_kb    == 1024);
    CHECK(cfg.telegram.connection_retry_count_max == 5);
    CHECK(cfg.telegram.prefer_ipv6               == false);
    CHECK(cfg.telegram.upload_parallelism        == 4);

    // Aria2.
    CHECK(cfg.aria2.max_connection_per_server == 16);
    CHECK(cfg.aria2.split                     == 16);
    CHECK(cfg.aria2.min_split_size            == "1M");
    CHECK(cfg.aria2.disk_cache                == "128M");
    CHECK(cfg.aria2.max_tries                 == 5);
    CHECK(cfg.aria2.retry_wait                == std::chrono::seconds{5});
    CHECK(cfg.aria2.max_overall_download_limit == 0);
    CHECK(cfg.aria2.max_overall_upload_limit  == 0);
    CHECK(cfg.aria2.enable_dht                == true);
    CHECK(cfg.aria2.enable_pex                == true);
    CHECK(cfg.aria2.bt_max_peers              == 55);
    CHECK(cfg.aria2.user_agent                == "aria2/1.37.0");

    // qBittorrent.
    CHECK(cfg.qbittorrent.max_active_downloads        == 8);
    CHECK(cfg.qbittorrent.max_active_uploads          == 8);
    CHECK(cfg.qbittorrent.max_active_torrents         == 16);
    CHECK(cfg.qbittorrent.max_connections             == 500);
    CHECK(cfg.qbittorrent.max_connections_per_torrent == 100);
    CHECK(cfg.qbittorrent.max_uploads                 == 20);
    CHECK(cfg.qbittorrent.max_uploads_per_torrent     == 5);
    CHECK(cfg.qbittorrent.up_limit                    == 0);
    CHECK(cfg.qbittorrent.dl_limit                    == 0);
    CHECK(cfg.qbittorrent.dht                         == true);
    CHECK(cfg.qbittorrent.pex                         == true);
    CHECK(cfg.qbittorrent.lsd                         == true);
    CHECK(cfg.qbittorrent.anonymous_mode              == false);
    CHECK(cfg.qbittorrent.async_io_threads            == 8);
    CHECK(cfg.qbittorrent.disk_cache_mib              == 256);

    // Rclone.
    CHECK(cfg.rclone.transfers                == 8);
    CHECK(cfg.rclone.checkers                 == 16);
    CHECK(cfg.rclone.multi_thread_streams     == 4);
    CHECK(cfg.rclone.multi_thread_cutoff      == "250M");
    CHECK(cfg.rclone.drive_chunk_size         == "64M");
    CHECK(cfg.rclone.buffer_size              == "32M");
    CHECK(cfg.rclone.use_mmap                 == true);
    CHECK(cfg.rclone.fast_list                == true);
    CHECK(cfg.rclone.drive_acknowledge_abuse  == true);
    CHECK(cfg.rclone.log_level                == "NOTICE");
    CHECK(cfg.rclone.extra_args.empty());

    // Google Drive.
    CHECK(cfg.google_drive.parallel_chunks_per_file     == 4);
    CHECK(cfg.google_drive.parallel_files_per_directory == 4);
    CHECK(cfg.google_drive.max_retries                  == 6);
    CHECK(cfg.google_drive.initial_retry_delay
          == std::chrono::milliseconds{500});
}

TEST_CASE("Throughput env-var overrides reach the right field",
          "[core][configuration][throughput]") {
    clear_cmlb_env();
    {
        ScopedEnv tg_upchunk{"CMLB_TELEGRAM_UPLOAD_CHUNK_SIZE_KB", "4096"};
        ScopedEnv tg_par{"CMLB_TELEGRAM_UPLOAD_PARALLELISM", "8"};
        ScopedEnv tg_ipv6{"CMLB_TELEGRAM_PREFER_IPV6", "true"};
        ScopedEnv aria_split{"CMLB_ARIA2_SPLIT", "12"};
        ScopedEnv aria_dht{"CMLB_ARIA2_ENABLE_DHT", "false"};
        ScopedEnv qb_active{"CMLB_QBITTORRENT_MAX_ACTIVE_DOWNLOADS", "32"};
        ScopedEnv qb_cache{"CMLB_QBITTORRENT_DISK_CACHE_MIB", "1024"};
        ScopedEnv rc_tx{"CMLB_RCLONE_TRANSFERS", "16"};
        ScopedEnv rc_mmap{"CMLB_RCLONE_USE_MMAP", "false"};
        ScopedEnv gd_chunks{"CMLB_GOOGLE_DRIVE_PARALLEL_CHUNKS_PER_FILE", "8"};
        ScopedEnv gd_files{"CMLB_GOOGLE_DRIVE_PARALLEL_FILES_PER_DIRECTORY", "6"};
        ScopedEnv gd_delay{"CMLB_GOOGLE_DRIVE_INITIAL_RETRY_DELAY_MS", "750"};

        auto result = Configuration::from_json(valid_doc());
        REQUIRE(result.has_value());
        const AppConfig& cfg = *result;

        CHECK(cfg.telegram.upload_chunk_size_kb == 4096);
        CHECK(cfg.telegram.upload_parallelism   == 8);
        CHECK(cfg.telegram.prefer_ipv6          == true);
        CHECK(cfg.aria2.split                   == 12);
        CHECK(cfg.aria2.enable_dht              == false);
        CHECK(cfg.qbittorrent.max_active_downloads == 32);
        CHECK(cfg.qbittorrent.disk_cache_mib    == 1024);
        CHECK(cfg.rclone.transfers              == 16);
        CHECK(cfg.rclone.use_mmap               == false);
        CHECK(cfg.google_drive.parallel_chunks_per_file     == 8);
        CHECK(cfg.google_drive.parallel_files_per_directory == 6);
        CHECK(cfg.google_drive.initial_retry_delay
              == std::chrono::milliseconds{750});
    }
    // After scope, defaults restored.
    auto result = Configuration::from_json(valid_doc());
    REQUIRE(result.has_value());
    CHECK(result->aria2.split == 16);
    CHECK(result->rclone.transfers == 8);
    CHECK(result->google_drive.parallel_chunks_per_file == 4);
}

TEST_CASE("Throughput fields parse from JSON when explicitly set",
          "[core][configuration][throughput]") {
    clear_cmlb_env();
    auto doc = valid_doc();
    doc["telegram"]["upload_parallelism"]       = 16;
    doc["aria2"]["split"]                       = 8;
    doc["aria2"]["min_split_size"]              = "4M";
    doc["rclone"]["transfers"]                  = 32;
    doc["rclone"]["extra_args"]                 = nlohmann::json::array(
        {"--no-traverse", "--checksum"});
    doc["google_drive"]["parallel_chunks_per_file"]     = 2;
    doc["google_drive"]["parallel_files_per_directory"] = 12;
    doc["google_drive"]["initial_retry_delay"]          = 200;  // ms

    auto result = Configuration::from_json(doc);
    REQUIRE(result.has_value());
    const AppConfig& cfg = *result;

    CHECK(cfg.telegram.upload_parallelism == 16);
    CHECK(cfg.aria2.split                 == 8);
    CHECK(cfg.aria2.min_split_size        == "4M");
    CHECK(cfg.rclone.transfers            == 32);
    REQUIRE(cfg.rclone.extra_args.size()  == 2);
    CHECK(cfg.rclone.extra_args[0]        == "--no-traverse");
    CHECK(cfg.rclone.extra_args[1]        == "--checksum");
    CHECK(cfg.google_drive.parallel_chunks_per_file     == 2);
    CHECK(cfg.google_drive.parallel_files_per_directory == 12);
    CHECK(cfg.google_drive.initial_retry_delay
          == std::chrono::milliseconds{200});
}

// ---------------------------------------------------------------------------
// Validation gap closure — rejects misconfigurations that previously slipped
// past the validator and only manifested as obscure runtime errors.
// ---------------------------------------------------------------------------

TEST_CASE("qbittorrent.url scheme is validated",
          "[core][configuration][validate]") {
    clear_cmlb_env();
    SECTION("missing scheme") {
        auto doc = valid_doc();
        doc["qbittorrent"]["url"] = "localhost:8080";
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("qbittorrent.url"));
        CHECK_THAT(result.error().message, ContainsSubstring("http://"));
    }
    SECTION("ws:// is wrong scheme") {
        auto doc = valid_doc();
        doc["qbittorrent"]["url"] = "ws://localhost:8080";
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("qbittorrent.url"));
    }
    SECTION("https:// accepted") {
        auto doc = valid_doc();
        doc["qbittorrent"]["url"] = "https://qbit.example.com:8443";
        auto result = Configuration::from_json(doc);
        REQUIRE(result.has_value());
        CHECK(result->qbittorrent.url == "https://qbit.example.com:8443");
    }
}

TEST_CASE("qbittorrent up_limit / dl_limit accept -1 / 0 / >0 only",
          "[core][configuration][validate]") {
    clear_cmlb_env();
    SECTION("-2 rejected for up_limit") {
        auto doc = valid_doc();
        doc["qbittorrent"]["up_limit"] = -2;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("up_limit"));
    }
    SECTION("-1 accepted (unlimited)") {
        auto doc = valid_doc();
        doc["qbittorrent"]["up_limit"] = -1;
        doc["qbittorrent"]["dl_limit"] = -1;
        auto result = Configuration::from_json(doc);
        REQUIRE(result.has_value());
        CHECK(result->qbittorrent.up_limit == -1);
        CHECK(result->qbittorrent.dl_limit == -1);
    }
}

TEST_CASE("rclone parallelism upper bounds enforced",
          "[core][configuration][validate]") {
    clear_cmlb_env();
    SECTION("transfers > 64 rejected") {
        auto doc = valid_doc();
        doc["rclone"]["transfers"] = 999;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("rclone.transfers"));
        CHECK_THAT(result.error().message, ContainsSubstring("[1, 64]"));
    }
    SECTION("checkers > 256 rejected") {
        auto doc = valid_doc();
        doc["rclone"]["checkers"] = 9999;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message, ContainsSubstring("rclone.checkers"));
    }
    SECTION("multi_thread_streams > 64 rejected") {
        auto doc = valid_doc();
        doc["rclone"]["multi_thread_streams"] = 200;
        auto result = Configuration::from_json(doc);
        REQUIRE_FALSE(result.has_value());
        CHECK_THAT(result.error().message,
                   ContainsSubstring("rclone.multi_thread_streams"));
    }
}

TEST_CASE("database.busy_timeout has a 60 s upper bound",
          "[core][configuration][validate]") {
    clear_cmlb_env();
    auto doc = valid_doc();
    doc["database"]["busy_timeout"] = 120'000;  // 2 minutes
    auto result = Configuration::from_json(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK_THAT(result.error().message, ContainsSubstring("busy_timeout"));
    CHECK_THAT(result.error().message, ContainsSubstring("60000"));
}

TEST_CASE("Malformed env override is dropped without corrupting the value",
          "[core][configuration][env]") {
    clear_cmlb_env();
    {
        ScopedEnv api_id{"CMLB_TELEGRAM_API_ID", "not-a-number"};
        auto result = Configuration::from_json(valid_doc());
        // Validation still passes because the JSON value (12345) survives;
        // the warn is logged to spdlog but doesn't surface as a config error.
        REQUIRE(result.has_value());
        CHECK(result->telegram.api_id == 12345);
    }
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
