#pragma once

#include <span>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/download/downloader_interface.hpp>

/// @file progress_renderer_interface.hpp
/// @brief Port that lets application use cases push live status updates
///        without depending on the presentation layer's concrete renderer.
///
/// The concrete `cmlb::presentation::ProgressRenderer` implements this; use
/// cases construct against the interface so the application library has no
/// link-time dependency on `cmlb_presentation`.

namespace cmlb::application {

/// Sink for live status messages, scoped per chat.
///
/// Implementations are expected to coalesce/throttle updates and to manage
/// the lifecycle of the underlying status message (send-once-then-edit).
/// Use cases call `render()` on every status tick; the implementation
/// decides whether to actually edit the chat.
class ProgressRendererInterface {
public:
    ProgressRendererInterface()          = default;
    virtual ~ProgressRendererInterface() = default;

    ProgressRendererInterface(const ProgressRendererInterface&)            = delete;
    ProgressRendererInterface& operator=(const ProgressRendererInterface&) = delete;
    ProgressRendererInterface(ProgressRendererInterface&&)                 = delete;
    ProgressRendererInterface& operator=(ProgressRendererInterface&&)      = delete;

    /// Updates (or creates) the per-chat status message based on @p active.
    /// Implementations may no-op when called inside their throttle window or
    /// when the rendered HTML is unchanged.
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<void>>
    render(cmlb::domain::ChatId chat,
           std::span<const cmlb::infrastructure::download::DownloadStatus> active) = 0;

    /// Bypasses throttle + dedup and emits a fresh message. Used by the
    /// presentation layer when the user explicitly presses "Refresh".
    [[nodiscard]] virtual boost::asio::awaitable<cmlb::core::Result<void>>
    force_refresh(cmlb::domain::ChatId chat,
                  std::span<const cmlb::infrastructure::download::DownloadStatus> active) = 0;
};

}  // namespace cmlb::application
