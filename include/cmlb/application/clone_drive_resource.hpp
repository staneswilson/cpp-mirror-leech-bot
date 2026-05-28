#pragma once

#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/google_drive_uploader.hpp>

/// @file clone_drive_resource.hpp
/// @brief CloneDriveResource use case — server-side GDrive copy.

namespace cmlb::application {

struct CloneRequest {
    /// Public Drive URL or bare file/folder id.
    std::string source_url;
    cmlb::domain::UserId user;
    cmlb::domain::ChatId chat;
};

class CloneDriveResource {
public:
    CloneDriveResource(cmlb::infrastructure::upload::GoogleDriveUploader& gdrive,
                       cmlb::infrastructure::telegram::MessengerInterface& messenger,
                       std::string target_folder_id) noexcept;

    /// Returns the new Drive id of the top-level copy.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::string>> execute(
        CloneRequest request);

private:
    cmlb::infrastructure::upload::GoogleDriveUploader& gdrive_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    std::string target_folder_id_;
};

} // namespace cmlb::application
