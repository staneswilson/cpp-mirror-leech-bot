#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>

namespace cmlb::infrastructure::download {

/// qBittorrent backend. Talks to the Web API v2 over HTTP, sharing the
/// process-wide `BeastHttpClient` connection pool.
///
/// Authentication: the first request performs `POST /api/v2/auth/login`
/// with the configured credentials and captures the `SID` cookie. Every
/// subsequent request attaches that cookie. A 403 response triggers a
/// transparent re-login + retry once per call.
class QbittorrentDownloader final : public DownloaderInterface {
public:
    /// Constructs the client. The HTTP client must outlive this instance.
    QbittorrentDownloader(cmlb::core::Executor& executor,
                          cmlb::core::QbittorrentConfig config,
                          cmlb::infrastructure::http::BeastHttpClient& http_client);

    ~QbittorrentDownloader() override;

    QbittorrentDownloader(const QbittorrentDownloader&) = delete;
    QbittorrentDownloader& operator=(const QbittorrentDownloader&) = delete;
    QbittorrentDownloader(QbittorrentDownloader&&) = delete;
    QbittorrentDownloader& operator=(QbittorrentDownloader&&) = delete;

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_uri(
        std::string_view uri, DownloadOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_torrent(
        std::span<const std::byte> torrent_data, DownloadOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<void>> pause(cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<void>> resume(cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<void>> remove(cmlb::domain::Gid id,
                                                            bool delete_files) override;

    boost::asio::awaitable<cmlb::core::Result<DownloadStatus>> status(
        cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<std::vector<DownloadStatus>>> active() override;

    boost::asio::awaitable<cmlb::core::Result<GlobalStats>> global_stats() override;

    [[nodiscard]] bool is_connected() const noexcept override;
    [[nodiscard]] std::string_view client_name() const noexcept override;

private:
    /// Pimpl shielding the cookie state, mutex, and recent-hash bookkeeping
    /// (qBittorrent's `add` returns no id — we diff `info` to recover one).
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cmlb::infrastructure::download
