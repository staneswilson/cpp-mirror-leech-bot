#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/domain/upload_destination.hpp>

/// @file user_settings_repository.hpp
/// @brief Per-user preferences persisted in the `user_settings` table.

namespace cmlb::infrastructure::persistence {

/// Plain data structure mirroring a single `user_settings` row. Optional
/// fields map to nullable columns. Strong-typed wrappers (UserId,
/// UploadDestination) are used at the field level to prevent confusion.
struct UserSettingsRecord {
    domain::UserId             user_id;
    domain::UploadDestination  leech_destination{domain::UploadDestination::Telegram};
    domain::UploadDestination  mirror_destination{domain::UploadDestination::GoogleDrive};
    std::optional<std::string> default_thumb_path;
    std::optional<std::string> rclone_remote;
    std::optional<std::string> gdrive_folder_id;
    bool                       upload_as_document{false};
    std::chrono::system_clock::time_point created_at{};
    std::chrono::system_clock::time_point updated_at{};
};

class UserSettingsRepository {
public:
    virtual ~UserSettingsRepository() = default;

    UserSettingsRepository()                                         = default;
    UserSettingsRepository(const UserSettingsRepository&)            = delete;
    UserSettingsRepository& operator=(const UserSettingsRepository&) = delete;
    UserSettingsRepository(UserSettingsRepository&&)                 = delete;
    UserSettingsRepository& operator=(UserSettingsRepository&&)      = delete;

    /// Fetches the row for @p user, or `nullopt` if none exists.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<std::optional<UserSettingsRecord>>>
    get(domain::UserId user) = 0;

    /// Insert-or-replace by `user_id`. Refreshes `updated_at` on success.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>>
    save(UserSettingsRecord record) = 0;

    /// Deletes the row for @p user. Returns `NotFound` if no row matched.
    [[nodiscard]] virtual boost::asio::awaitable<core::Result<void>>
    remove(domain::UserId user) = 0;
};

}  // namespace cmlb::infrastructure::persistence
