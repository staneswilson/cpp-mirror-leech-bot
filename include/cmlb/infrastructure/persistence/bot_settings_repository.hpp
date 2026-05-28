#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>

/// @file bot_settings_repository.hpp
/// @brief Singleton aggregate for runtime-tunable bot settings.
///
/// Unlike `AppConfig` (loaded from disk, immutable for the lifetime of the
/// process), `BotSettingsRecord` is mutable at runtime through owner-only
/// commands and is persisted between restarts in the `bot_settings` table.

namespace cmlb::infrastructure::persistence {

struct BotSettingsRecord {
    std::int64_t owner_id{0};
    std::vector<std::int64_t> sudo_users;
    std::vector<std::int64_t> authorized_chats;
    std::filesystem::path download_dir{"downloads"};
    std::int64_t leech_split_size{2'000'000'000}; ///< Telegram cap, bytes.
    std::int64_t upload_limit_bytes{0};           ///< 0 = unlimited.
    std::chrono::milliseconds status_update_interval{5000};
    std::chrono::milliseconds rss_poll_interval{60'000};
    std::chrono::system_clock::time_point updated_at{};
};

class BotSettingsRepository {
public:
    virtual ~BotSettingsRepository() = default;

    BotSettingsRepository() = default;
    BotSettingsRepository(const BotSettingsRepository&) = delete;
    BotSettingsRepository& operator=(const BotSettingsRepository&) = delete;
    BotSettingsRepository(BotSettingsRepository&&) = delete;
    BotSettingsRepository& operator=(BotSettingsRepository&&) = delete;

    /// Loads the singleton row. If no row exists yet, returns a record
    /// populated with default values (without writing anything).
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<BotSettingsRecord>> load() = 0;

    /// Saves (insert-or-replace) the singleton row at id = 1. Refreshes
    /// `updated_at` on success.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>> save(
        BotSettingsRecord record) = 0;
};

} // namespace cmlb::infrastructure::persistence
