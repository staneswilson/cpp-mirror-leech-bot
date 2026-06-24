#pragma once

#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/drive_resource_operations.hpp>

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
    CountDriveResource(cmlb::infrastructure::upload::DriveResourceOperations& gdrive,
                       cmlb::infrastructure::telegram::MessengerInterface& messenger) noexcept;

    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<cmlb::infrastructure::upload::CountResult>>
    execute(CountRequest request);

private:
    cmlb::infrastructure::upload::DriveResourceOperations& gdrive_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
};

} // namespace cmlb::application
