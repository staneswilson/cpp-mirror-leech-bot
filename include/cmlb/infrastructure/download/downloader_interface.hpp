#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>

namespace cmlb::infrastructure::download {

/// Coarse lifecycle states common to every supported downloader backend.
/// Backend-specific sub-states are normalized into this enum at the adapter
/// boundary so callers can branch uniformly.
enum class DownloadState {
    Queued,
    Downloading,
    Paused,
    Complete,
    Error,
    Seeding,
    Removed
};

/// Returns the canonical lowercase string for a `DownloadState`. Used in
/// log lines and rendered status messages.
[[nodiscard]] std::string_view to_string(DownloadState state) noexcept;

/// Snapshot of a single download as reported by the underlying backend.
///
/// All counters are absolute (not deltas). Optional fields are populated when
/// the backend exposes them (e.g. `seed_ratio` only for torrents).
struct DownloadStatus {
    /// Backend-assigned identifier (aria2 GID or qBittorrent info-hash).
    cmlb::domain::Gid id;
    /// Display name (filename, torrent name, or magnet's `dn` parameter).
    std::string name;
    /// Normalized lifecycle state.
    DownloadState state{DownloadState::Queued};

    /// Total size of the payload in bytes (0 if unknown).
    std::int64_t total_bytes{0};
    /// Bytes downloaded so far.
    std::int64_t downloaded_bytes{0};
    /// Bytes uploaded so far (torrents only — 0 otherwise).
    std::int64_t uploaded_bytes{0};
    /// Instantaneous download rate in bytes per second.
    std::int64_t download_speed_bps{0};
    /// Instantaneous upload rate in bytes per second.
    std::int64_t upload_speed_bps{0};

    /// Estimated time to completion. Zero if unknown or already complete.
    std::chrono::seconds eta{0};

    /// Backend error message when `state == Error`.
    std::optional<std::string> error_message;
    /// Filesystem destination directory (where files are/were written).
    std::optional<std::filesystem::path> save_path;
    /// Absolute paths of every file produced by this download.
    std::vector<std::filesystem::path> files;

    /// Torrent: current upload/download ratio.
    std::optional<float> seed_ratio;
    /// Torrent: connected seeders count.
    std::optional<int> num_seeders;
    /// Torrent: connected leechers count.
    std::optional<int> num_leechers;

    /// Returns the completion percentage in `[0, 100]`. Returns `0.0` when
    /// `total_bytes` is unknown.
    [[nodiscard]] double progress() const noexcept;
};

/// Caller-supplied tunables for `add_uri` / `add_torrent`. `extras` carries
/// backend-specific key/value pairs that are forwarded verbatim (aria2
/// options or qBittorrent multipart fields).
struct DownloadOptions {
    /// Override the configured default download directory.
    std::optional<std::filesystem::path> save_directory;
    /// Backend-specific opaque option pairs.
    std::vector<std::pair<std::string, std::string>> extras;
};

/// Aggregate statistics across every active download in the backend.
struct GlobalStats {
    /// Sum of `download_speed_bps` across active downloads.
    std::int64_t download_speed_bps{0};
    /// Sum of `upload_speed_bps` across active downloads.
    std::int64_t upload_speed_bps{0};
    /// Number of currently downloading items.
    int active_count{0};
    /// Number of queued / waiting items.
    int waiting_count{0};
    /// Number of stopped / paused / errored items.
    int stopped_count{0};
};

/// Abstract downloader port. Concrete adapters wrap aria2 (JSON-RPC over
/// WebSocket) and qBittorrent (HTTP Web API v2). Every operation is
/// cancellation-aware: cancelling the awaitable cleanly aborts the
/// in-flight RPC.
class DownloaderInterface {
public:
    virtual ~DownloaderInterface() = default;

    DownloaderInterface(const DownloaderInterface&) = delete;
    DownloaderInterface& operator=(const DownloaderInterface&) = delete;
    DownloaderInterface(DownloaderInterface&&) = delete;
    DownloaderInterface& operator=(DownloaderInterface&&) = delete;

    /// Enqueues a URL (HTTP/FTP/magnet) for download. Returns the new id.
    virtual boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_uri(
        std::string_view uri, DownloadOptions options) = 0;

    /// Enqueues a torrent given its raw `.torrent` bytes.
    virtual boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_torrent(
        std::span<const std::byte> torrent_data, DownloadOptions options) = 0;

    /// Pauses an active download.
    virtual boost::asio::awaitable<cmlb::core::Result<void>> pause(cmlb::domain::Gid id) = 0;

    /// Resumes a paused download.
    virtual boost::asio::awaitable<cmlb::core::Result<void>> resume(cmlb::domain::Gid id) = 0;

    /// Removes a download. When `delete_files` is true the on-disk payload
    /// is also unlinked.
    virtual boost::asio::awaitable<cmlb::core::Result<void>> remove(cmlb::domain::Gid id,
                                                                    bool delete_files) = 0;

    /// Fetches the latest status snapshot for a single download.
    virtual boost::asio::awaitable<cmlb::core::Result<DownloadStatus>> status(
        cmlb::domain::Gid id) = 0;

    /// Fetches every currently active (non-terminal) download.
    virtual boost::asio::awaitable<cmlb::core::Result<std::vector<DownloadStatus>>> active() = 0;

    /// Backend-wide stats (aggregate rates, queue lengths).
    virtual boost::asio::awaitable<cmlb::core::Result<GlobalStats>> global_stats() = 0;

    /// True when the underlying transport is connected and ready to issue
    /// RPCs.
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /// Short identifier of the concrete backend (`"aria2"`, `"qbittorrent"`).
    [[nodiscard]] virtual std::string_view client_name() const noexcept = 0;

    /// True when the backend reports per-file completion incrementally (via
    /// `DownloadStatus.files` growing as bytes land on disk), so the caller
    /// can pipeline uploads against an in-flight download. Aria2 returns true
    /// here; qBittorrent returns false because torrents need to seed after
    /// the payload completes — kicking off uploads mid-download breaks the
    /// seed-to-ratio guarantee. Defaults to `false`; adapters opt in.
    [[nodiscard]] virtual bool supports_pipelining() const noexcept {
        return false;
    }

protected:
    DownloaderInterface() = default;
};

} // namespace cmlb::infrastructure::download
