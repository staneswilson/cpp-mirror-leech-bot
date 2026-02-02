#ifndef CMLB_DOWNLOADERS_ARIA2_CLIENT_HPP
#define CMLB_DOWNLOADERS_ARIA2_CLIENT_HPP

#include "downloaders/idownloader.hpp"
#include <memory>

namespace cmlb {

/**
 * @brief Aria2 JSON-RPC client implementing IDownloader interface.
 * 
 * Connects to aria2c daemon via WebSocket and provides async download management.
 * Supports automatic reconnection with exponential backoff.
 */
class Aria2Downloader : public IDownloader {
public:
    struct Config {
        std::string rpc_url = "ws://localhost:6800/jsonrpc";
        std::string secret;
        std::chrono::milliseconds connect_timeout{5000};
        int max_reconnect_attempts{10};
    };

    explicit Aria2Downloader(const Config& config);
    ~Aria2Downloader() override;

    // Non-copyable, non-movable (due to internal state)
    Aria2Downloader(const Aria2Downloader&) = delete;
    Aria2Downloader& operator=(const Aria2Downloader&) = delete;

    // ========================================================================
    // IDownloader Implementation
    // ========================================================================

    std::future<Result<std::string>> addUri(
        std::string_view uri,
        const std::vector<std::pair<std::string, std::string>>& options = {}
    ) override;

    std::future<Result<std::string>> addTorrent(
        std::string_view torrent_data,
        const std::vector<std::pair<std::string, std::string>>& options = {}
    ) override;

    std::future<Result<void>> pause(std::string_view id) override;
    std::future<Result<void>> resume(std::string_view id) override;
    std::future<Result<void>> remove(std::string_view id, bool delete_files = false) override;

    std::future<Result<DownloadStatus>> getStatus(std::string_view id) override;
    std::future<Result<std::vector<DownloadStatus>>> getActiveDownloads() override;
    std::future<Result<GlobalStats>> getGlobalStats() override;

    [[nodiscard]] bool isConnected() const noexcept override;
    [[nodiscard]] std::string_view clientName() const noexcept override { return "aria2"; }

    // ========================================================================
    // Aria2-Specific Methods
    // ========================================================================

    /**
     * @brief Force pause (even if download is actively writing).
     */
    std::future<Result<void>> forcePause(std::string_view gid);

    /**
     * @brief Force remove (even if download is actively writing).
     */
    std::future<Result<void>> forceRemove(std::string_view gid);

    /**
     * @brief Get aria2 version info.
     */
    std::future<Result<std::string>> getVersion();

    /**
     * @brief Change global options at runtime.
     * @param options Key-value pairs (e.g., {"max-overall-download-limit": "1M"})
     */
    std::future<Result<void>> changeGlobalOptions(
        const std::vector<std::pair<std::string, std::string>>& options
    );

    /**
     * @brief Shutdown aria2 daemon (use with caution).
     */
    std::future<Result<void>> shutdown();

    // Factory method
    static std::unique_ptr<Aria2Downloader> create(const Config& config);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cmlb

#endif // CMLB_DOWNLOADERS_ARIA2_CLIENT_HPP
