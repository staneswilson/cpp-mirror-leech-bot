#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::core {

namespace {

constexpr const char* kLoggerName = "cmlb";
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";
constexpr std::size_t kThreadPoolQueue = 8192;
constexpr std::size_t kThreadPoolThreads = 1;

} // namespace

Result<spdlog::level::level_enum> parse_log_level(std::string_view text) {
    if (text == "trace")
        return spdlog::level::trace;
    if (text == "debug")
        return spdlog::level::debug;
    if (text == "info")
        return spdlog::level::info;
    if (text == "warn" || text == "warning")
        return spdlog::level::warn;
    if (text == "error" || text == "err")
        return spdlog::level::err;
    if (text == "critical" || text == "fatal")
        return spdlog::level::critical;
    if (text == "off")
        return spdlog::level::off;
    return ::cmlb::core::error(ErrorCode::InvalidArgument,
                               std::string{"unknown log level: "} + std::string{text});
}

Result<void> Logger::initialize(const LogConfig& config) {
    try {
        const auto level = parse_log_level(config.level);
        if (!level) {
            return std::unexpected(level.error());
        }

        std::error_code ec;
        std::filesystem::create_directories(config.logs_dir, ec);
        if (ec) {
            return ::cmlb::core::error(ErrorCode::FileSystem,
                                       "failed to create logs dir '" + config.logs_dir.string()
                                           + "': " + ec.message());
        }

        // Replace any previously installed default logger / thread pool first.
        spdlog::shutdown();
        spdlog::init_thread_pool(kThreadPoolQueue, kThreadPoolThreads);

        std::vector<spdlog::sink_ptr> sinks;

        const auto file_path = config.logs_dir / "cmlb.log";
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file_path.string(), config.rotating_file_max_bytes, config.rotating_file_max_files);
        sinks.push_back(std::move(file_sink));

        if (config.console) {
            auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            sinks.push_back(std::move(console_sink));
        }

        auto logger = std::make_shared<spdlog::async_logger>(kLoggerName,
                                                             sinks.begin(),
                                                             sinks.end(),
                                                             spdlog::thread_pool(),
                                                             spdlog::async_overflow_policy::block);

        logger->set_pattern(kPattern);
        logger->set_level(*level);
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(logger);
        spdlog::set_level(*level);
        return {};
    } catch (const std::exception& ex) {
        return ::cmlb::core::error(ErrorCode::Internal,
                                   std::string{"spdlog initialization failed: "} + ex.what());
    } catch (...) {
        return ::cmlb::core::error(ErrorCode::Internal,
                                   "spdlog initialization failed: unknown exception");
    }
}

void Logger::shutdown() noexcept {
    try {
        spdlog::default_logger()->flush();
    } catch (...) {
        // best effort
    }
    try {
        spdlog::shutdown();
    } catch (...) {
        // best effort — nothing useful to log to at this point
    }
}

} // namespace cmlb::core
