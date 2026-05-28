#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <nlohmann/json.hpp>

#include <cmlb/core/configuration.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/core/executor.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>

namespace cmlb::infrastructure::download {

/// aria2 backend. Speaks JSON-RPC 2.0 over a single persistent WebSocket
/// connection (`ws://` or `wss://`). On any disconnect the client schedules
/// a reconnect with exponential backoff capped at 30 s.
///
/// Concurrency model: the underlying WebSocket I/O lives on a private
/// strand. Public methods are safe to call from any executor — each one
/// posts onto the strand, then `co_await`s a future-like completion that
/// fires when the matching JSON-RPC response is received.
class Aria2Downloader final : public DownloaderInterface {
public:
    /// Constructs the client. The WebSocket connection is established
    /// lazily on first use, then maintained in the background.
    Aria2Downloader(cmlb::core::Executor& executor,
                    cmlb::core::Aria2Config config);

    ~Aria2Downloader() override;

    Aria2Downloader(const Aria2Downloader&)            = delete;
    Aria2Downloader& operator=(const Aria2Downloader&) = delete;
    Aria2Downloader(Aria2Downloader&&)                 = delete;
    Aria2Downloader& operator=(Aria2Downloader&&)      = delete;

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>>
        add_uri(std::string_view uri, DownloadOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>>
        add_torrent(std::span<const std::byte> torrent_data,
                    DownloadOptions options) override;

    boost::asio::awaitable<cmlb::core::Result<void>>
        pause(cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<void>>
        resume(cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<void>>
        remove(cmlb::domain::Gid id, bool delete_files) override;

    boost::asio::awaitable<cmlb::core::Result<DownloadStatus>>
        status(cmlb::domain::Gid id) override;

    boost::asio::awaitable<cmlb::core::Result<std::vector<DownloadStatus>>>
        active() override;

    boost::asio::awaitable<cmlb::core::Result<GlobalStats>>
        global_stats() override;

    [[nodiscard]] bool is_connected() const noexcept override;
    [[nodiscard]] std::string_view client_name() const noexcept override;

    /// Aria2 reports per-file completion via `bittorrent.completed` and the
    /// generic `files[].completedLength == files[].length` check, so uploads
    /// can stream alongside the download.
    [[nodiscard]] bool supports_pipelining() const noexcept override { return true; }

private:
    /// Pimpl: hides Boost.Beast types from the public header. The
    /// implementation owns the WebSocket stream, its strand, the pending
    /// request map, the reconnect timer, and the cancel signal. Held by
    /// `shared_ptr` so the background read loop can extend its own lifetime
    /// via `shared_from_this()`.
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace cmlb::infrastructure::download
