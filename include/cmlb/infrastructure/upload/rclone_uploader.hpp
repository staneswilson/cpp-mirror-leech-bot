#pragma once

#include <atomic>
#include <filesystem>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

/// @file rclone_uploader.hpp
/// @brief `UploaderInterface` adapter that shells out to the `rclone` binary
///        via `infrastructure::system::Subprocess`.

namespace cmlb::core {
class Executor;
}

namespace cmlb::infrastructure::system {
class Subprocess;
}

namespace cmlb::infrastructure::upload {

/// rclone mirror adapter.
///
/// `--progress --stats=1s --stats-one-line` output is parsed line-by-line to
/// drive `UploadProgress` callbacks. No retry logic lives here — rclone has
/// its own `--retries` flag and that is the right place to tune it.
class RcloneUploader final : public UploaderInterface {
public:
    /// @param exec        Executor used for any timers/throttling.
    /// @param config      rclone-section configuration (executable path,
    ///                    `--config` file path).
    /// @param subprocess  Shared subprocess launcher. The uploader does not
    ///                    own it.
    RcloneUploader(cmlb::core::Executor& exec,
                   cmlb::core::RcloneConfig config,
                   cmlb::infrastructure::system::Subprocess& subprocess);

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>>
    upload_file(std::filesystem::path path,
                UploadConfig config,
                UploadProgressHandler on_progress) override;

    [[nodiscard]] boost::asio::awaitable<
        cmlb::core::Result<std::vector<UploadResult>>>
    upload_directory(std::filesystem::path path,
                     UploadConfig config,
                     UploadProgressHandler on_progress) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "rclone"; }

    /// Cached result of `rclone --version` (probed lazily on the first call).
    [[nodiscard]] bool is_ready() const noexcept override;

private:
    /// Probes `rclone --version` once and caches the verdict.
    [[nodiscard]] boost::asio::awaitable<bool> probe_rclone() const;

    /// Common driver shared by `upload_file` / `upload_directory`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>>
    run_rclone(std::vector<std::string> args,
               std::string display_name,
               UploadProgressHandler on_progress);

    cmlb::core::Executor&                    exec_;
    cmlb::core::RcloneConfig                 config_;
    cmlb::infrastructure::system::Subprocess& subprocess_;

    mutable std::atomic<int> readiness_{-1};  // -1=unprobed, 0=not ready, 1=ready
};

}  // namespace cmlb::infrastructure::upload
