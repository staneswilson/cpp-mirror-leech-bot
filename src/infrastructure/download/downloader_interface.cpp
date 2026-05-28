// ---------------------------------------------------------------------------
// src/infrastructure/download/downloader_interface.cpp
//
// Out-of-line definitions for the small, backend-agnostic helpers declared in
// `downloader_interface.hpp` — `to_string(DownloadState)` and
// `DownloadStatus::progress()`. Kept here to avoid pulling them into every
// translation unit that includes the header.
// ---------------------------------------------------------------------------

#include <cmlb/infrastructure/download/downloader_interface.hpp>

namespace cmlb::infrastructure::download {

std::string_view to_string(DownloadState state) noexcept {
    switch (state) {
        case DownloadState::Queued:      return "queued";
        case DownloadState::Downloading: return "downloading";
        case DownloadState::Paused:      return "paused";
        case DownloadState::Complete:    return "complete";
        case DownloadState::Error:       return "error";
        case DownloadState::Seeding:     return "seeding";
        case DownloadState::Removed:     return "removed";
    }
    return "unknown";
}

double DownloadStatus::progress() const noexcept {
    if (total_bytes <= 0) return 0.0;
    const auto pct =
        (static_cast<double>(downloaded_bytes) /
         static_cast<double>(total_bytes)) *
        100.0;
    if (pct < 0.0)   return 0.0;
    if (pct > 100.0) return 100.0;
    return pct;
}

}  // namespace cmlb::infrastructure::download
