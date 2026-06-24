#pragma once

#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/drive_resource_operations.hpp>

/// @file delete_drive_resource.hpp
/// @brief DeleteDriveResource use case — Drive `files.delete` wrapper.

namespace cmlb::application {

struct DeleteDriveRequest {
    std::string source_url;
    cmlb::domain::UserId user;
    cmlb::domain::ChatId chat;
};

class DeleteDriveResource {
public:
    DeleteDriveResource(cmlb::infrastructure::upload::DriveResourceOperations& gdrive,
                        cmlb::infrastructure::telegram::MessengerInterface& messenger) noexcept;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> execute(
        DeleteDriveRequest request);

private:
    cmlb::infrastructure::upload::DriveResourceOperations& gdrive_;
    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
};

} // namespace cmlb::application
