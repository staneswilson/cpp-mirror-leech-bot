#pragma once

#include <cstdint>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>

/// @file drive_resource_operations.hpp
/// @brief Narrow Google Drive file/folder operations used by command use cases.

namespace cmlb::infrastructure::upload {

/// Recursive count summary for a Drive file/folder tree.
struct CountResult {
    int files{0};
    int folders{0};
    std::int64_t total_bytes{0};
};

/// Port for server-side Drive resource operations outside upload workflows.
class DriveResourceOperations {
public:
    DriveResourceOperations() = default;
    virtual ~DriveResourceOperations() = default;

    DriveResourceOperations(const DriveResourceOperations&) = delete;
    DriveResourceOperations& operator=(const DriveResourceOperations&) = delete;
    DriveResourceOperations(DriveResourceOperations&&) = delete;
    DriveResourceOperations& operator=(DriveResourceOperations&&) = delete;

    /// Copies @p source_id under @p target_folder_id and returns the new top-level id.
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<std::string>> copy(
        std::string source_id, std::string target_folder_id) = 0;

    /// Recursively counts files, folders, and bytes under @p folder_id.
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<CountResult>> count(
        std::string folder_id) = 0;

    /// Deletes the Drive resource identified by @p file_id.
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<void>> remove(
        std::string file_id) = 0;
};

} // namespace cmlb::infrastructure::upload
