#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include <cmlb/core/error.hpp>

namespace cmlb::core {

/// Mirror of `spdlog::level::level_enum` for callers that don't want to depend
/// on the spdlog enum directly.
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off
};

/// Parses a textual log level (`"trace"`, `"debug"`, ...) into spdlog's enum.
[[nodiscard]] Result<spdlog::level::level_enum> parse_log_level(std::string_view text);

/// Configuration for `Logger::initialize`.
struct LogConfig {
    /// Directory the rotating file sink writes into (created if missing).
    std::filesystem::path logs_dir;
    /// Textual level — passed through `parse_log_level`.
    std::string level{"info"};
    /// Emit a colored stderr sink in addition to the file sink.
    bool console{true};
    /// Per-file rotation threshold in bytes (default 10 MiB).
    std::size_t rotating_file_max_bytes{10 * 1024 * 1024};
    /// Number of rotated files to keep (default 5).
    std::size_t rotating_file_max_files{5};
};

/// Thin static facade over `spdlog` providing typed, format-string-checked
/// log macros and lifecycle (`initialize`/`shutdown`) helpers.
///
/// The actual logger is the spdlog default logger; this class only owns the
/// initialization pattern (async thread pool + rotating file + stderr).
class Logger {
public:
    Logger() = delete;

    /// Initializes the global async logger using `config`. Idempotent —
    /// calling it twice replaces the previous default logger.
    [[nodiscard]] static Result<void> initialize(const LogConfig& config);

    /// Flushes and drops the global logger. Safe to call when not initialized.
    static void shutdown() noexcept;

    /// Logs at the corresponding level using `spdlog`'s compile-time checked
    /// format-string overloads.
    template <typename... Args>
    static void trace(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::trace(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::debug(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::info(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::warn(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::error(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void critical(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        spdlog::critical(fmt, std::forward<Args>(args)...);
    }
};

}  // namespace cmlb::core
