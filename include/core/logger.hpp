#ifndef CMLB_CORE_LOGGER_HPP
#define CMLB_CORE_LOGGER_HPP

#include <print>
#include <mutex>
#include <string_view>
#include <chrono>
#include <format>

namespace cmlb {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static void log(LogLevel level, std::string_view message) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
        std::string_view level_str = "";
        
        switch (level) {
            case LogLevel::DEBUG: level_str = "DEBUG"; break;
            case LogLevel::INFO:  level_str = "INFO "; break;
            case LogLevel::WARN:  level_str = "WARN "; break;
            case LogLevel::ERROR: level_str = "ERROR"; break;
        }

        // Using simple formatting. In C++23 std::print is ideal.
        std::println(stderr, "[{:%H:%M:%S}] [{}] {}", now, level_str, message);
    }

    template <typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::DEBUG, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::INFO, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::WARN, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::ERROR, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    static inline std::mutex mutex_;
};

} // namespace cmlb

#endif // CMLB_CORE_LOGGER_HPP
