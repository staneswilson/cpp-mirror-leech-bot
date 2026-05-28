#pragma once

#include <functional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/bot_settings_repository.hpp>

/// @file update_bot_settings.hpp
/// @brief UpdateBotSettings use case — owner-only runtime config mutation.
///
/// Authority is checked by the command dispatcher (not here). This use case
/// loads the singleton record, applies the mutator, and persists it.

namespace cmlb::application {

using BotSettingsMutator =
    std::function<void(cmlb::infrastructure::persistence::BotSettingsRecord&)>;

struct UpdateBotSettingsRequest {
    BotSettingsMutator mutate;
};

class UpdateBotSettings {
public:
    explicit UpdateBotSettings(
        cmlb::infrastructure::persistence::BotSettingsRepository& repo) noexcept;

    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<cmlb::infrastructure::persistence::BotSettingsRecord>>
    execute(UpdateBotSettingsRequest request);

private:
    cmlb::infrastructure::persistence::BotSettingsRepository& repo_;
};

}  // namespace cmlb::application
