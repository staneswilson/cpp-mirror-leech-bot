#pragma once

#include <functional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/persistence/user_settings_repository.hpp>

/// @file update_user_settings.hpp
/// @brief UpdateUserSettings use case — load-or-default, mutate, save.

namespace cmlb::application {

/// Mutation callable type. Receives a mutable reference to the record about
/// to be persisted. Pure mutation only — must not throw.
using UserSettingsMutator =
    std::function<void(cmlb::infrastructure::persistence::UserSettingsRecord&)>;

struct UpdateUserSettingsRequest {
    cmlb::domain::UserId user;
    /// Applied to the record after loading (or constructing a default).
    UserSettingsMutator mutate;
};

class UpdateUserSettings {
public:
    explicit UpdateUserSettings(
        cmlb::infrastructure::persistence::UserSettingsRepository& repo) noexcept;

    /// Returns the post-mutation record on success.
    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<cmlb::infrastructure::persistence::UserSettingsRecord>>
    execute(UpdateUserSettingsRequest request);

private:
    cmlb::infrastructure::persistence::UserSettingsRepository& repo_;
};

} // namespace cmlb::application
