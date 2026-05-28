// ---------------------------------------------------------------------------
// progress_renderer.cpp
//
// Manages the live status message per chat. Sends-once-then-edits with
// throttling and content-hash dedup. Edit failures fall back to a fresh send.
//
// Concurrency model
// -----------------
// Each chat owns a dedicated `boost::asio::strand`. The public `render()` /
// `force_refresh()` methods `co_spawn` their work onto that strand, so the
// inner coroutine's *associated executor* IS the strand — every inner
// `co_await` (messenger, timers, semaphore) resumes on the strand. This is
// the load-bearing property: a one-shot `asio::post(bind_executor(strand,
// ...))` hop is NOT enough, because the outer coroutine's associated executor
// is set at co_spawn time and the next async wait would resume on whichever
// executor that was, leaving subsequent state writes off-strand.
//
// The strand alone still doesn't prevent two coroutines from both observing
// `cached_id==0` between their respective awaits, so each impl additionally
// acquires the per-chat `RenderSemaphore` (a 1-slot `experimental::channel`
// used as an async mutex). RAII guard releases the token on scope exit:
//   * Periodic `render()` coalesces via `try_receive` — drops on contention;
//     the next polling tick (≤ throttle window) will re-render with fresh
//     data.
//   * `force_refresh()` waits via `async_receive` — it's user-initiated via
//     the Refresh button and silently dropping is bad UX. Caller-side
//     cancellation propagates naturally as an exception from `async_receive`.
// ---------------------------------------------------------------------------

#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <cmlb/core/logger.hpp>
#include <cmlb/presentation/html_renderer.hpp>
#include <cmlb/presentation/progress_renderer.hpp>

namespace cmlb::presentation {

namespace asio = boost::asio;
using cmlb::core::Logger;
using cmlb::core::Result;

namespace {

using SemChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;

/// RAII release for the per-chat render semaphore. `try_send` is
/// non-blocking and always succeeds on a capacity-1 channel whose token we
/// previously held. Runs on the chat's strand because the owning coroutine's
/// associated executor IS the strand — no extra synchronization needed.
struct SemReleaseGuard {
    SemChannel& sem;

    ~SemReleaseGuard() {
        sem.try_send(boost::system::error_code{});
    }
};

} // namespace

ProgressRenderer::ProgressRenderer(cmlb::infrastructure::telegram::MessengerInterface& messenger,
                                   cmlb::infrastructure::system::SystemMetrics& metrics,
                                   std::chrono::steady_clock::time_point bot_start_time,
                                   boost::asio::any_io_executor executor,
                                   std::chrono::milliseconds throttle)
    : messenger_{messenger},
      metrics_{metrics},
      bot_start_time_{bot_start_time},
      executor_{std::move(executor)},
      throttle_{throttle} {
}

ProgressRenderer::~ProgressRenderer() = default;

ProgressRenderer::ChatState& ProgressRenderer::chat_state(cmlb::domain::ChatId chat) {
    // Get-or-create under the map mutex. ChatState lives on the heap
    // (unique_ptr) so the reference we return remains valid across
    // subsequent rehashes of `chats_` — callers cache it.
    std::scoped_lock guard{mutex_};
    auto& slot = chats_[chat];
    if (!slot) {
        slot = std::make_unique<ChatState>();
        slot->strand =
            std::make_shared<asio::strand<asio::any_io_executor>>(asio::make_strand(executor_));
        // Construct the 1-slot async mutex on the chat's strand and pre-fill
        // its single token so the first acquirer succeeds immediately. Safe
        // to mutate from here: no other coroutine has yet observed `*slot`.
        slot->sem = std::make_unique<RenderSemaphore>(*slot->strand, 1);
        slot->sem->try_send(boost::system::error_code{});
    }
    return *slot;
}

asio::awaitable<Result<void>> ProgressRenderer::render(
    cmlb::domain::ChatId chat,
    std::span<const cmlb::infrastructure::download::DownloadStatus> active) {
    auto& state = chat_state(chat);
    auto strand = state.strand;
    co_return co_await asio::co_spawn(
        *strand, do_render_impl(state, chat, active), asio::use_awaitable);
}

asio::awaitable<Result<void>> ProgressRenderer::do_render_impl(
    ChatState& state,
    cmlb::domain::ChatId chat,
    std::span<const cmlb::infrastructure::download::DownloadStatus> active) {
    // Running with associated executor = state.strand. All state field
    // reads/writes and every co_await below are strand-serialized.

    // ----- 0. Coalesce concurrent renders ---------------------------------
    // Non-blocking acquire: `try_receive` returns true iff it consumed a
    // token. If false, another render is mid-flight — drop and rely on the
    // next polling tick (the throttle window bounds staleness).
    const bool acquired = state.sem->try_receive([](boost::system::error_code, auto&&...) {
    });
    if (!acquired) {
        co_return Result<void>{};
    }
    SemReleaseGuard guard{*state.sem};

    // ----- 1. Build the current rendering ---------------------------------
    const auto snapshot = metrics_.snapshot();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - bot_start_time_);

    std::string html = active.empty() ? HtmlRenderer::render_no_active_tasks(snapshot, uptime)
                                      : HtmlRenderer::render_status(active, snapshot, uptime);

    const auto now = std::chrono::steady_clock::now();
    const bool have_message = state.status_message_id.value() != 0;

    // ----- 2. Throttle: skip if within throttle window --------------------
    if (have_message && (now - state.last_edit) < throttle_) {
        co_return Result<void>{};
    }

    // ----- 3. Dedup: skip if content unchanged ----------------------------
    if (have_message && html == state.last_rendered_html) {
        co_return Result<void>{};
    }

    // ----- 4. Either edit the existing message or send a new one ----------
    if (!have_message) {
        auto send = co_await messenger_.send_html_with_keyboard(
            chat,
            html,
            cmlb::infrastructure::telegram::Messenger::refresh_close_row("status:refresh"));
        if (!send) {
            co_return std::unexpected(send.error());
        }
        state.status_message_id = *send;
        state.last_edit = now;
        state.last_rendered_html = std::move(html);
        co_return Result<void>{};
    }

    const auto cached_id = state.status_message_id;
    auto edit = co_await messenger_.edit_html(chat, cached_id, html);
    if (!edit) {
        Logger::debug("progress: edit failed for chat={} (msg={}): {}. Sending fresh message.",
                      chat.value(),
                      cached_id.value(),
                      edit.error().message);
        auto send = co_await messenger_.send_html_with_keyboard(
            chat,
            html,
            cmlb::infrastructure::telegram::Messenger::refresh_close_row("status:refresh"));
        if (!send) {
            co_return std::unexpected(send.error());
        }
        state.status_message_id = *send;
        state.last_edit = now;
        state.last_rendered_html = std::move(html);
        co_return Result<void>{};
    }

    state.last_edit = now;
    state.last_rendered_html = std::move(html);
    co_return Result<void>{};
}

asio::awaitable<Result<void>> ProgressRenderer::force_refresh(
    cmlb::domain::ChatId chat,
    std::span<const cmlb::infrastructure::download::DownloadStatus> active) {
    auto& state = chat_state(chat);
    auto strand = state.strand;
    co_return co_await asio::co_spawn(
        *strand, do_force_refresh_impl(state, chat, active), asio::use_awaitable);
}

asio::awaitable<Result<void>> ProgressRenderer::do_force_refresh_impl(
    ChatState& state,
    cmlb::domain::ChatId chat,
    std::span<const cmlb::infrastructure::download::DownloadStatus> active) {
    // Blocking acquire: wait until the in-flight render releases the token.
    // Force-refresh is user-clicked, so dropping is bad UX. The await suspends
    // on the channel rather than busy-polling, and caller-side cancellation
    // propagates naturally as a thrown exception out of `async_receive`,
    // unwinding before any state writes — exactly what we want.
    co_await state.sem->async_receive(asio::use_awaitable);
    SemReleaseGuard guard{*state.sem};

    const auto snapshot = metrics_.snapshot();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - bot_start_time_);

    std::string html = active.empty() ? HtmlRenderer::render_no_active_tasks(snapshot, uptime)
                                      : HtmlRenderer::render_status(active, snapshot, uptime);

    auto send = co_await messenger_.send_html_with_keyboard(
        chat, html, cmlb::infrastructure::telegram::Messenger::refresh_close_row("status:refresh"));
    if (!send) {
        co_return std::unexpected(send.error());
    }

    state.status_message_id = *send;
    state.last_edit = std::chrono::steady_clock::now();
    state.last_rendered_html = std::move(html);
    co_return Result<void>{};
}

} // namespace cmlb::presentation
