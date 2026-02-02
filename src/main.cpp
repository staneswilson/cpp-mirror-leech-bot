#include "core/bot_engine.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"

#include <print>
#include <csignal>
#include <atomic>

namespace {
    // Atomic flag for async-signal-safe shutdown
    std::atomic<bool> g_shutdown_requested{false};
    cmlb::BotEngine* g_engine = nullptr;
}

/**
 * @brief Async-signal-safe signal handler.
 * 
 * Only sets an atomic flag. No I/O, no mutex locks, no memory allocation.
 * This is the ONLY safe way to handle signals in a multithreaded C++ program.
 */
extern "C" void signal_handler(int signum) {
    g_shutdown_requested.store(true, std::memory_order_release);
    
    // requestStop() only sets an atomic bool, which is safe
    if (g_engine) {
        g_engine->requestStop();
    }
    
    // For SIGTERM, we might want to exit more aggressively
    if (signum == SIGTERM) {
        std::_Exit(0);
    }
}

int main(int argc, char* argv[]) {
    try {
        std::string config_path = "config.json";
        if (argc > 1) {
            config_path = argv[1];
        }

        auto config_res = cmlb::AppConfig::load(config_path);
        if (!config_res) {
            std::println(stderr, "Fatal configuration error: {}", config_res.error().message);
            std::println(stderr, "Please create '{}' based on config.example.json", config_path);
            return 1;
        }

        cmlb::Logger::info("Initializing Telegram Mirror Bot (C++23)...");
        
        cmlb::BotEngine engine(*config_res);
        g_engine = &engine;

        // Register signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        engine.run();
        
        cmlb::Logger::info("Shutdown complete.");
        
    } catch (const std::exception& e) {
        cmlb::Logger::error("Fatal Error: {}", e.what());
        return 1;
    } catch (...) {
        cmlb::Logger::error("Unknown Fatal Error");
        return 2;
    }

    return 0;
}
