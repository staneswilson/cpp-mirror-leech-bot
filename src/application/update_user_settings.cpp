// ---------------------------------------------------------------------------
// update_user_settings.cpp — UpdateUserSettings use case implementation.
// ---------------------------------------------------------------------------

#include <cmlb/application/update_user_settings.hpp>

#include <chrono>
#include <utility>

#include <cmlb/core/logger.hpp>

namespace cmlb::application {

namespace asio = boost::asio;

UpdateUserSettings::UpdateUserSettings(
    cmlb::infrastructure::persistence::UserSettingsRepository& repo) noexcept
    : repo_{repo} {}

asio::awaitable<cmlb::core::Result<
    cmlb::infrastructure::persistence::UserSettingsRecord>>
UpdateUserSettings::execute(UpdateUserSettingsRequest request) {
    cmlb::core::Logger::info("update_user_settings: user={}",
                             request.user.value());

    if (!request.mutate) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "update_user_settings: null mutator");
    }

    auto loaded = co_await repo_.get(request.user);
    if (!loaded) co_return std::unexpected(loaded.error());

    const auto now = std::chrono::system_clock::now();
    cmlb::infrastructure::persistence::UserSettingsRecord rec;
    if (loaded->has_value()) {
        rec = std::move(**loaded);
    } else {
        rec.user_id    = request.user;
        rec.created_at = now;
    }
    rec.updated_at = now;

    request.mutate(rec);

    auto saved = co_await repo_.save(rec);
    if (!saved) co_return std::unexpected(saved.error());

    cmlb::core::Logger::info("update_user_settings: user={} saved",
                             request.user.value());
    co_return rec;
}

}  // namespace cmlb::application
