#pragma once

#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/google_drive_uploader.hpp>

/// @file count_drive_resource.hpp
/// @brief CountDriveResource use case — recursive count of files/folders/bytes.

namespace cmlb::application {

struct CountRequest {
    std::string source_url;
    cmlb::domain::UserId user;
    cmlb::domain::ChatId chat;
};

class CountDriveResource {
public:
    CountDriveResource(cmlb::infrastructure::upload::GoogleDriveUploader& gdrive,
                       cmlb::infrastructure::telegram::MessengerInterface& messenger) noexcept;

    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<cmlb::infrastructure::upload::CountResult>>
    execute(CountRequest request);

private:
    cmlb::infrastructure::upload::GoogleDriveUploader& gdrive_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
};

} // namespace cmlb::application
