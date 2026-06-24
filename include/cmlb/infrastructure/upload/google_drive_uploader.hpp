#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/upload/drive_resource_operations.hpp>
#include <cmlb/infrastructure/upload/uploader_interface.hpp>

/// @file google_drive_uploader.hpp
/// @brief `UploaderInterface` adapter targeting Google Drive via the v3 REST
///        API. Authenticates with a service-account RS256 JWT — no OAuth user
///        consent flow.

namespace cmlb::core {
class Executor;
} // namespace cmlb::core

namespace cmlb::infrastructure::http {
class BeastHttpClient;
} // namespace cmlb::infrastructure::http

namespace cmlb::infrastructure::upload {

/// Cached OAuth bearer token obtained by exchanging a service-account JWT.
struct DriveAccessToken {
    /// `"Bearer <token>"` ready to drop into an `Authorization` header.
    std::string value;
    /// Wall-clock expiry. The uploader refreshes 60 s before this point.
    std::chrono::steady_clock::time_point expires_at{};
};

/// Service-account credentials parsed from `credentials_path`. Holds only the
/// fields we need; the rest of the JSON document is ignored.
struct ServiceAccountKey {
    std::string client_email;
    /// PEM-encoded RSA private key (PKCS#8) as supplied by Google.
    std::string private_key_pem;
    std::string token_uri{"https://oauth2.googleapis.com/token"};
};

/// Google Drive mirror adapter.
class GoogleDriveUploader final : public UploaderInterface, public DriveResourceOperations {
public:
    /// @param exec         Executor for any internally-spawned timers.
    /// @param config       Drive-section configuration (parent folder, chunk
    ///                     size, credentials path, ...).
    /// @param http_client  Shared Beast HTTP client; the uploader does not
    ///                     own it.
    GoogleDriveUploader(cmlb::core::Executor& exec,
                        cmlb::core::GoogleDriveConfig config,
                        cmlb::infrastructure::http::BeastHttpClient& http_client);

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>> upload_file(
        std::filesystem::path path,
        UploadConfig config,
        UploadProgressHandler on_progress) override;

    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::vector<UploadResult>>>
    upload_directory(std::filesystem::path path,
                     UploadConfig config,
                     UploadProgressHandler on_progress) override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "gdrive";
    }

    /// True once the service-account key file parsed cleanly during
    /// construction.
    [[nodiscard]] bool is_ready() const noexcept override {
        return ready_;
    }

    // ------------------------------------------------------------------
    // Extra Drive operations needed by the application layer (clone /
    // count / delete use cases). Implemented in terms of the Drive v3
    // REST API using the same bearer-token cache used by uploads.
    // ------------------------------------------------------------------

    /// Server-side copy of @p source_id into @p target_folder_id. For folder
    /// sources the copy is recursive (the implementation walks the source
    /// tree and re-issues `files.copy` for each file, recreating folder
    /// structure as it goes). Returns the new id of the top-level copy.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::string>> copy(
        std::string source_id, std::string target_folder_id) override;

    /// Recursive count of every file and folder reachable from @p folder_id
    /// plus the sum of all file byte sizes. The folder itself is not counted.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<CountResult>> count(
        std::string folder_id) override;

    /// Permanently deletes the file or folder identified by @p file_id (the
    /// Drive `files.delete` endpoint — moves to the user's trash for
    /// `My Drive`; permanently deletes for shared drives).
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> remove(
        std::string file_id) override;

private:
    /// Returns a cached or freshly-minted bearer token. Refresh threshold is
    /// 60 s before expiry. Thread-safe; guarded by `token_mutex_`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::string>> acquire_bearer();

    /// Performs the JWT-grant exchange. Caller must hold `token_mutex_`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<DriveAccessToken>> mint_bearer();

    /// Drops the cached token so the next `acquire_bearer()` call forces a
    /// fresh mint. Called by long-running parallel uploads on a 401 response
    /// so all sibling workers reload on their next iteration.
    void invalidate_bearer_cache() noexcept;

    /// Builds a signed RS256 JWT for the configured service account.
    [[nodiscard]] cmlb::core::Result<std::string> build_signed_jwt() const;

    /// Multipart upload for files <= 5 MiB.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>> upload_multipart(
        std::filesystem::path path,
        const std::string& parent_folder,
        UploadProgressHandler on_progress);

    /// Resumable upload for files > 5 MiB. Aborts the session on cancellation.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<UploadResult>> upload_resumable(
        std::filesystem::path path,
        const std::string& parent_folder,
        UploadProgressHandler on_progress);

    /// Creates a `application/vnd.google-apps.folder` entry under `parent` and
    /// returns its id.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::string>> create_folder(
        std::string name, std::string parent);

    /// DELETEs an abandoned resumable session URI. Best-effort: errors are
    /// logged but do not propagate to the caller.
    boost::asio::awaitable<void> abort_resumable_session(std::string session_uri);

    [[maybe_unused]] cmlb::core::Executor& exec_;
    cmlb::core::GoogleDriveConfig config_;
    cmlb::infrastructure::http::BeastHttpClient& http_;

    ServiceAccountKey key_{};
    bool ready_{false};

    mutable std::mutex token_mutex_;
    DriveAccessToken cached_token_{};
};

} // namespace cmlb::infrastructure::upload
