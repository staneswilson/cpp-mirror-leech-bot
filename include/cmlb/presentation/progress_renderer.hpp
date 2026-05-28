#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/application/progress_renderer_interface.hpp>
#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>
#include <cmlb/infrastructure/system/system_metrics.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>

/// @file progress_renderer.hpp
/// @brief Owner of the live "status message" lifecycle.
///
/// For each chat the renderer keeps:
///  - The MessageId of the previously-sent status message (or 0 if none yet).
///  - The timestamp of the last successful edit.
///  - The exact HTML it last rendered, so identical updates can be skipped.
///
/// `render()` is the high-level entry point: send-once-then-edit semantics
/// with built-in throttling and content deduplication. `force_refresh()`
/// bypasses the cache (used when a user explicitly presses Refresh).

namespace cmlb::presentation {

/// Per-chat live status message manager. Implements the application-layer
/// `ProgressRendererInterface` port so use cases can depend on the interface
/// without pulling in the presentation layer.
class ProgressRenderer final : public cmlb::application::ProgressRendererInterface {
public:
    /// @param messenger        Used for `send_html_with_keyboard` and `edit_html`.
    /// @param metrics          Sampled on every render to build the footer.
    /// @param bot_start_time   Used to compute the bot uptime in the footer.
    /// @param executor         Base executor used to construct per-chat strands.
    ///                         All renders for a given chat are serialized on
    ///                         the chat's strand, eliminating races between
    ///                         concurrent tasks that share a chat.
    /// @param throttle         Minimum interval between edits per-chat. Updates
    ///                         issued within this window are coalesced.
    ProgressRenderer(cmlb::infrastructure::telegram::MessengerInterface& messenger,
                     cmlb::infrastructure::system::SystemMetrics&        metrics,
                     std::chrono::steady_clock::time_point               bot_start_time,
                     boost::asio::any_io_executor                        executor,
                     std::chrono::milliseconds                           throttle =
                         std::chrono::seconds{3});

    ~ProgressRenderer() override;

    /// Updates (or creates) the per-chat status message.
    ///
    /// Side-effects (depend on cached state):
    ///  - First call for @p chat: sends a new message with the refresh/close
    ///    keyboard and records its MessageId.
    ///  - Subsequent call within the throttle window: returns immediately.
    ///  - Subsequent call after the throttle window with unchanged HTML:
    ///    returns immediately.
    ///  - Otherwise: edits the cached message. If the edit fails (e.g. user
    ///    deleted it), sends a fresh message and updates the cache.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> render(
        cmlb::domain::ChatId chat,
        std::span<const cmlb::infrastructure::download::DownloadStatus> active) override;

    /// Like `render()` but always emits a fresh message, discarding the
    /// throttle and dedup checks for this call. The cached state is rewritten.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> force_refresh(
        cmlb::domain::ChatId chat,
        std::span<const cmlb::infrastructure::download::DownloadStatus> active) override;

private:
    /// Single-token async semaphore. Capacity 1: holding the token means
    /// "render in progress." `try_receive` is the non-blocking acquire used
    /// by periodic `render()` (coalesce on contention); `async_receive` is
    /// the blocking acquire used by `force_refresh()` (wait, because the user
    /// clicked Refresh and dropping is bad UX). Release is `try_send` from an
    /// RAII guard — `try_send` is non-blocking and always succeeds on a
    /// previously-acquired channel of capacity 1.
    using RenderSemaphore =
        boost::asio::experimental::channel<void(boost::system::error_code)>;

    /// Cached per-chat state. All mutable fields except `strand` and `sem`
    /// are accessed only while the calling coroutine is dispatched on
    /// `strand`, so they need no further synchronization.
    struct ChatState {
        cmlb::domain::MessageId                status_message_id{0};
        std::chrono::steady_clock::time_point  last_edit{};
        std::string                            last_rendered_html;
        /// Per-chat serialization strand. shared_ptr so the strand keeps the
        /// chat's `any_io_executor` alive for as long as any render coroutine
        /// suspended on it. Lifetime is otherwise bounded by ProgressRenderer.
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand;
        /// Async mutex protecting the "a render is in flight" critical
        /// section. Lives on the chat's strand executor. Constructed in the
        /// "unlocked" state by `try_send`-ing one token during chat_state()
        /// initialization. heap-allocated so the address survives map
        /// rehashes (callers cache the parent `ChatState*`).
        std::unique_ptr<RenderSemaphore>       sem;
    };

    /// Returns the ChatState for `chat`, creating it on first access. The
    /// strand is bound to `executor_` so all renders for `chat` serialize
    /// through it. Caller must NOT touch returned state while off-strand —
    /// hop to `state.strand` first, then mutate freely without further locking.
    ChatState& chat_state(cmlb::domain::ChatId chat);

    /// Body of `render()`, co_spawned on the chat's strand so that its
    /// *associated executor* is the strand — every inner `co_await` resumes
    /// on the strand, making `state` field reads/writes between awaits
    /// strand-serialized. Coalesces against `state.render_in_progress`.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> do_render_impl(
        ChatState&                                                      state,
        cmlb::domain::ChatId                                            chat,
        std::span<const cmlb::infrastructure::download::DownloadStatus> active);

    /// Body of `force_refresh()`, co_spawned on the chat's strand. Waits
    /// for any in-flight render to complete instead of coalescing, because
    /// a user pressing Refresh expects a visible response.
    [[nodiscard]] boost::asio::awaitable<cmlb::core::Result<void>> do_force_refresh_impl(
        ChatState&                                                      state,
        cmlb::domain::ChatId                                            chat,
        std::span<const cmlb::infrastructure::download::DownloadStatus> active);

    cmlb::infrastructure::telegram::MessengerInterface& messenger_;
    cmlb::infrastructure::system::SystemMetrics&        metrics_;
    std::chrono::steady_clock::time_point        bot_start_time_;
    boost::asio::any_io_executor                 executor_;
    std::chrono::milliseconds                    throttle_;

    /// Guards `chats_` insertion / lookup only — per-chat state mutations
    /// happen on the chat's strand. ChatState is heap-allocated so that the
    /// reference returned by `chat_state()` remains valid across rehashes —
    /// callers cache the address and access fields freely while on-strand.
    std::mutex mutex_;
    std::unordered_map<cmlb::domain::ChatId,
                       std::unique_ptr<ChatState>,
                       std::hash<cmlb::domain::ChatId>> chats_;
};

}  // namespace cmlb::presentation
