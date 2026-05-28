#include <cmlb/infrastructure/upload/rclone_uploader.hpp>

#include <chrono>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <fmt/format.h>

#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

namespace cmlb::infrastructure::upload {

namespace {

namespace fs = std::filesystem;

using cmlb::infrastructure::system::Subprocess;
using cmlb::infrastructure::system::SubprocessRequest;

// ---------------------------------------------------------------------------
// Progress-line parser.
//
// Sample `rclone --progress --stats=1s --stats-one-line` line:
//
//   "Transferred:   12.34 MiB / 100.00 MiB, 12%, 1.23 MiB/s, ETA 1m10s"
//
// `rclone` is permissive about whitespace and unit casing; the regex below
// accepts the variants we have observed in the wild.
// ---------------------------------------------------------------------------

struct ParsedProgress {
    std::int64_t uploaded_bytes{0};
    std::int64_t total_bytes{0};
    std::int64_t speed_bps{0};
    std::chrono::seconds eta{0};
    bool valid{false};
};

[[nodiscard]] double unit_to_factor(std::string_view unit) {
    if (unit == "B" || unit.empty())  return 1.0;
    if (unit == "KiB" || unit == "K") return 1024.0;
    if (unit == "MiB" || unit == "M") return 1024.0 * 1024;
    if (unit == "GiB" || unit == "G") return 1024.0 * 1024 * 1024;
    if (unit == "TiB" || unit == "T") return 1024.0 * 1024 * 1024 * 1024.0;
    return 1.0;
}

[[nodiscard]] std::chrono::seconds parse_eta(std::string_view text) {
    // "1m10s", "45s", "2h3m", "10s", "-" => 0
    if (text.empty() || text == "-") return std::chrono::seconds{0};
    std::int64_t total = 0;
    std::int64_t num   = 0;
    bool have_num      = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            have_num = true;
        } else if (have_num) {
            switch (c) {
                case 'h': total += num * 3600; break;
                case 'm': total += num * 60;   break;
                case 's': total += num;        break;
                default: break;
            }
            num = 0;
            have_num = false;
        }
    }
    if (have_num) total += num;  // bare seconds
    return std::chrono::seconds{total};
}

/// Joins an rclone `"remote:path"` prefix with a basename, inserting a `/`
/// only when the remote spec doesn't already end on a separator or colon.
[[nodiscard]] std::string join_remote(std::string_view base,
                                      std::string_view leaf) {
    std::string out{base};
    if (!out.empty() && out.back() != '/' && out.back() != ':') out += '/';
    out.append(leaf);
    return out;
}

/// Builds the common throughput / progress flag block consumed by every
/// rclone invocation we drive. Reads tunables from `RcloneConfig` (PR 4)
/// so operators control them via `config.json` / `CMLB_RCLONE_*` env vars.
///
/// Flag order is well-known flags first, `--config` next (only if set),
/// then `extra_args` last — late flags override earlier ones in rclone.
[[nodiscard]] std::vector<std::string>
build_common_rclone_args(const cmlb::core::RcloneConfig& cfg) {
    std::vector<std::string> args;
    args.reserve(24 + cfg.extra_args.size());

    args.emplace_back("--progress");
    args.emplace_back("--stats=1s");
    args.emplace_back("--stats-one-line");

    args.emplace_back("--transfers=" + std::to_string(cfg.transfers));
    args.emplace_back("--checkers=" + std::to_string(cfg.checkers));
    args.emplace_back("--multi-thread-streams="
                      + std::to_string(cfg.multi_thread_streams));
    args.emplace_back("--multi-thread-cutoff=" + cfg.multi_thread_cutoff);
    args.emplace_back("--drive-chunk-size=" + cfg.drive_chunk_size);
    args.emplace_back("--buffer-size=" + cfg.buffer_size);

    if (cfg.use_mmap)                 args.emplace_back("--use-mmap");
    if (cfg.fast_list)                args.emplace_back("--fast-list");
    if (cfg.drive_acknowledge_abuse)  args.emplace_back("--drive-acknowledge-abuse");

    args.emplace_back("--log-level=" + cfg.log_level);

    if (!cfg.config_path.empty()) {
        args.emplace_back("--config");
        args.emplace_back(cfg.config_path.string());
    }

    for (const auto& extra : cfg.extra_args) {
        args.emplace_back(extra);
    }
    return args;
}

[[nodiscard]] ParsedProgress parse_line(std::string_view line) {
    // Cheap rejection.
    if (line.find("Transferred:") == std::string_view::npos) return {};

    static const std::regex kRe{
        R"(Transferred:\s*([0-9.]+)\s*([KMGTP]?i?B)?\s*/\s*([0-9.]+)\s*([KMGTP]?i?B)?,\s*\d+%,\s*([0-9.]+)\s*([KMGTP]?i?B)?/s(?:,\s*ETA\s*([0-9hms\-]+))?)"};

    std::cmatch m;
    if (!std::regex_search(line.data(), line.data() + line.size(), m, kRe)) {
        return {};
    }

    ParsedProgress p;
    try {
        p.uploaded_bytes = static_cast<std::int64_t>(
            std::stod(m[1].str()) * unit_to_factor(m[2].str()));
        p.total_bytes = static_cast<std::int64_t>(
            std::stod(m[3].str()) * unit_to_factor(m[4].str()));
        p.speed_bps = static_cast<std::int64_t>(
            std::stod(m[5].str()) * unit_to_factor(m[6].str()));
        if (m[7].matched) p.eta = parse_eta(m[7].str());
        p.valid = true;
    } catch (...) {
        return {};
    }
    return p;
}

}  // namespace

// --------------------------------------------------------------------------
// RcloneUploader
// --------------------------------------------------------------------------

RcloneUploader::RcloneUploader(cmlb::core::Executor& exec,
                               cmlb::core::RcloneConfig config,
                               Subprocess& subprocess)
    : exec_{exec}, config_{std::move(config)}, subprocess_{subprocess} {}

bool RcloneUploader::is_ready() const noexcept {
    return readiness_.load(std::memory_order_acquire) == 1;
}

boost::asio::awaitable<bool> RcloneUploader::probe_rclone() const {
    int cached = readiness_.load(std::memory_order_acquire);
    if (cached >= 0) co_return cached == 1;

    SubprocessRequest req;
    req.executable = config_.executable;
    req.arguments  ={"--version"};

    auto res = co_await subprocess_.run(std::move(req));
    const bool ok = res && res->exit_code == 0;
    readiness_.store(ok ? 1 : 0, std::memory_order_release);
    co_return ok;
}

boost::asio::awaitable<cmlb::core::Result<UploadResult>>
RcloneUploader::run_rclone(std::vector<std::string> args,
                           std::string display_name,
                           UploadProgressHandler on_progress) {
    namespace asio = boost::asio;
    const auto started = std::chrono::steady_clock::now();

    if (!co_await probe_rclone()) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::RcloneInvocation,
            "rclone: binary unavailable (probe failed)");
    }
    if ((co_await asio::this_coro::cancellation_state).cancelled()
        != asio::cancellation_type::none) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled,
                                    "rclone: cancelled");
    }

    // Throttle progress callbacks to <= 1/s.
    auto last_cb = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    std::int64_t total_seen = 0;

    SubprocessRequest req;
    req.executable = config_.executable;
    req.arguments  =std::move(args);
    req.on_stdout_line = [&](std::string_view line) {
        auto p = parse_line(line);
        if (!p.valid) return;
        total_seen = p.total_bytes;
        const auto now = std::chrono::steady_clock::now();
        if (!on_progress) return;
        if ((now - last_cb) < std::chrono::seconds(1)) return;
        last_cb = now;
        UploadProgress prog;
        prog.file_name      = display_name;
        prog.total_bytes    = p.total_bytes;
        prog.uploaded_bytes = p.uploaded_bytes;
        prog.speed_bps      = p.speed_bps;
        prog.eta            = p.eta;
        on_progress(prog);
    };

    auto res = co_await subprocess_.run(std::move(req));
    if (!res) co_return std::unexpected(res.error());
    if (res->exit_code != 0) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::RcloneInvocation,
            fmt::format("rclone: exited {}: {}", res->exit_code,
                        res->stderr_data));
    }

    UploadResult out;
    out.file_id  = display_name;
    out.size     = total_seen;
    out.duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    co_return out;
}

boost::asio::awaitable<cmlb::core::Result<UploadResult>>
RcloneUploader::upload_file(fs::path path,
                            UploadConfig config,
                            UploadProgressHandler on_progress) {
    if (!config.rclone_path.has_value() || config.rclone_path->empty()) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::InvalidArgument,
            "rclone: UploadConfig.rclone_path is required (\"remote:path\")");
    }
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                    "rclone: file not found: " + path.string());
    }

    const std::string dest =
        join_remote(*config.rclone_path, path.filename().string());

    std::vector<std::string> args{"copyto", path.string(), dest};
    {
        auto common = build_common_rclone_args(config_);
        args.insert(args.end(),
                    std::make_move_iterator(common.begin()),
                    std::make_move_iterator(common.end()));
    }

    co_return co_await run_rclone(
        std::move(args), path.filename().string(), std::move(on_progress));
}

boost::asio::awaitable<cmlb::core::Result<std::vector<UploadResult>>>
RcloneUploader::upload_directory(fs::path path,
                                 UploadConfig config,
                                 UploadProgressHandler on_progress) {
    if (!config.rclone_path.has_value() || config.rclone_path->empty()) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::InvalidArgument,
            "rclone: UploadConfig.rclone_path is required (\"remote:path\")");
    }
    std::error_code ec;
    if (!fs::is_directory(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "rclone: not a directory: "
                                        + path.string());
    }

    const std::string dest =
        join_remote(*config.rclone_path, path.filename().string());

    std::vector<std::string> args{"copy", path.string(), dest};
    {
        auto common = build_common_rclone_args(config_);
        args.insert(args.end(),
                    std::make_move_iterator(common.begin()),
                    std::make_move_iterator(common.end()));
    }

    auto single = co_await run_rclone(
        std::move(args), path.filename().string(), std::move(on_progress));
    if (!single) co_return std::unexpected(single.error());

    std::vector<UploadResult> out;
    out.push_back(std::move(*single));
    co_return out;
}

}  // namespace cmlb::infrastructure::upload
