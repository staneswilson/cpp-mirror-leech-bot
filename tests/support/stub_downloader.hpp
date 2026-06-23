#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>

namespace cmlb::test_support {

/// Programmable in-memory DownloaderInterface used by application-layer
/// unit tests. Every call is recorded and any subsequent `status()` call
/// returns from a queue of pre-staged snapshots so the mirror/leech poll
/// loop can be exercised deterministically.
class StubDownloader final : public cmlb::infrastructure::download::DownloaderInterface {
public:
    struct AddCall {
        std::string uri;
        cmlb::infrastructure::download::DownloadOptions options;
    };

    StubDownloader() = default;
    ~StubDownloader() override = default;

    // ---- programming surface ------------------------------------------
    void set_next_gid(cmlb::domain::Gid gid) {
        std::lock_guard lk{mutex_};
        next_gid_ = std::move(gid);
    }

    void set_add_error(cmlb::core::AppError err) {
        std::lock_guard lk{mutex_};
        add_error_ = std::move(err);
    }

    void push_status(cmlb::infrastructure::download::DownloadStatus status) {
        std::lock_guard lk{mutex_};
        statuses_.push_back(std::move(status));
    }

    void set_status_error(cmlb::core::AppError err) {
        std::lock_guard lk{mutex_};
        status_error_ = std::move(err);
    }

    void set_global_stats(cmlb::infrastructure::download::GlobalStats stats) {
        std::lock_guard lk{mutex_};
        global_stats_ = stats;
    }

    void set_global_stats_error(cmlb::core::AppError err) {
        std::lock_guard lk{mutex_};
        global_stats_error_ = std::move(err);
    }

    /// Toggle the value returned by `supports_pipelining()`. Defaults to
    /// true so that application-layer tests exercise the aria2-style
    /// per-file pipelined upload path (the common case).
    void set_supports_pipelining(bool value) noexcept {
        supports_pipelining_.store(value, std::memory_order_relaxed);
    }

    // ---- inspection ----------------------------------------------------
    [[nodiscard]] std::vector<AddCall> add_calls() const {
        std::lock_guard lk{mutex_};
        return add_calls_;
    }

    [[nodiscard]] int pause_calls() const {
        std::lock_guard lk{mutex_};
        return pause_calls_;
    }

    [[nodiscard]] int resume_calls() const {
        std::lock_guard lk{mutex_};
        return resume_calls_;
    }

    [[nodiscard]] int remove_calls() const {
        std::lock_guard lk{mutex_};
        return remove_calls_;
    }

    // ---- DownloaderInterface ------------------------------------------
    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_uri(
        std::string_view uri, cmlb::infrastructure::download::DownloadOptions options) override {
        {
            std::lock_guard lk{mutex_};
            add_calls_.push_back(AddCall{std::string{uri}, std::move(options)});
            if (add_error_) {
                cmlb::core::AppError err = *add_error_;
                co_return std::unexpected(err);
            }
        }
        co_return next_gid_;
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::domain::Gid>> add_torrent(
        std::span<const std::byte>, cmlb::infrastructure::download::DownloadOptions) override {
        co_return next_gid_;
    }

    boost::asio::awaitable<cmlb::core::Result<void>> pause(cmlb::domain::Gid) override {
        std::lock_guard lk{mutex_};
        ++pause_calls_;
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>> resume(cmlb::domain::Gid) override {
        std::lock_guard lk{mutex_};
        ++resume_calls_;
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<void>> remove(cmlb::domain::Gid, bool) override {
        std::lock_guard lk{mutex_};
        ++remove_calls_;
        co_return cmlb::core::Result<void>{};
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::infrastructure::download::DownloadStatus>>
    status(cmlb::domain::Gid) override {
        std::lock_guard lk{mutex_};
        if (status_error_) {
            cmlb::core::AppError err = *status_error_;
            co_return std::unexpected(err);
        }
        if (statuses_.empty()) {
            cmlb::infrastructure::download::DownloadStatus s;
            s.state = cmlb::infrastructure::download::DownloadState::Complete;
            co_return s;
        }
        const auto front = statuses_.front();
        if (statuses_.size() > 1) {
            statuses_.erase(statuses_.begin());
        }
        co_return front;
    }

    boost::asio::awaitable<
        cmlb::core::Result<std::vector<cmlb::infrastructure::download::DownloadStatus>>>
    active() override {
        std::lock_guard lk{mutex_};
        co_return statuses_;
    }

    boost::asio::awaitable<cmlb::core::Result<cmlb::infrastructure::download::GlobalStats>>
    global_stats() override {
        std::lock_guard lk{mutex_};
        if (global_stats_error_) {
            cmlb::core::AppError err = *global_stats_error_;
            co_return std::unexpected(err);
        }
        co_return global_stats_;
    }

    [[nodiscard]] bool is_connected() const noexcept override {
        return true;
    }

    [[nodiscard]] std::string_view client_name() const noexcept override {
        return "stub";
    }

    [[nodiscard]] bool supports_pipelining() const noexcept override {
        return supports_pipelining_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    cmlb::domain::Gid next_gid_{std::string{"stub-gid"}};
    std::optional<cmlb::core::AppError> add_error_;
    std::optional<cmlb::core::AppError> status_error_;
    std::optional<cmlb::core::AppError> global_stats_error_;
    std::vector<cmlb::infrastructure::download::DownloadStatus> statuses_;
    std::vector<AddCall> add_calls_;
    int pause_calls_{0};
    int resume_calls_{0};
    int remove_calls_{0};
    cmlb::infrastructure::download::GlobalStats global_stats_;
    std::atomic<bool> supports_pipelining_{true};
};

} // namespace cmlb::test_support
