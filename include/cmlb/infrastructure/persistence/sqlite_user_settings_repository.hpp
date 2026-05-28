#pragma once

#include <optional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>

namespace cmlb::infrastructure::persistence {

class SqliteUserSettingsRepository final : public UserSettingsRepository {
public:
    explicit SqliteUserSettingsRepository(SqliteConnectionPool& pool) noexcept : pool_{pool} {
    }

    [[nodiscard]] boost::asio::awaitable<core::Result<std::optional<UserSettingsRecord>>> get(
        domain::UserId user) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>> save(
        UserSettingsRecord record) override;

    [[nodiscard]] boost::asio::awaitable<core::Result<void>> remove(domain::UserId user) override;

private:
    SqliteConnectionPool& pool_;
};

} // namespace cmlb::infrastructure::persistence
