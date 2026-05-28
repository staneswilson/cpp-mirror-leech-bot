// ---------------------------------------------------------------------------
// update_bot_settings.cpp — UpdateBotSettings use case implementation.
// ---------------------------------------------------------------------------

#include <chrono>
#include <utility>

#include <cmlb/application/update_bot_settings.hpp>
#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio = boost::asio;

UpdateBotSettings::UpdateBotSettings(
    cmlb::infrastructure::persistence::BotSettingsRepository& repo) noexcept
    : repo_{repo} {
}

asio::awaitable<cmlb::core::Result<cmlb::infrastructure::persistence::BotSettingsRecord>>
UpdateBotSettings::execute(UpdateBotSettingsRequest request) {
    cmlb::core::Logger::info("update_bot_settings: invoked");

    if (!request.mutate) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "update_bot_settings: null mutator");
    }

    auto loaded = co_await repo_.load();
    if (!loaded)
        co_return std::unexpected(loaded.error());

    cmlb::infrastructure::persistence::BotSettingsRecord rec = std::move(*loaded);
    request.mutate(rec);
    rec.updated_at = std::chrono::system_clock::now();

    auto saved = co_await repo_.save(rec);
    if (!saved)
        co_return std::unexpected(saved.error());

    cmlb::core::Logger::info("update_bot_settings: saved");
    co_return rec;
}

} // namespace cmlb::application
