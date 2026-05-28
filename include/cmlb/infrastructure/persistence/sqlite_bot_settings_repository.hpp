#pragma once

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

namespace cmlb::infrastructure::persistence {

class SqliteBotSettingsRepository final : public BotSettingsRepository {
public:
    explicit SqliteBotSettingsRepository(SqliteConnectionPool& pool) noexcept
        : pool_{pool} {}

    [[nodiscard]] boost::asio::awaitable<core::Result<BotSettingsRecord>>
    load() override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>>
    save(BotSettingsRecord record) override;

private:
    SqliteConnectionPool& pool_;
};

}  // namespace cmlb::infrastructure::persistence
