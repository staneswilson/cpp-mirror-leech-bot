#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>

/// @file media_processor_interface.hpp
/// @brief Abstract interface for inspecting and transforming video/audio
///        files. Concrete implementations shell out to ffmpeg/ffprobe.

namespace cmlb::infrastructure::media {

/// Compact subset of `ffprobe` output: just the fields the rest of CMLB
/// actually needs in order to upload, render previews, or pick a thumbnail.
struct MediaInfo {
    /// Container duration. Zero when the input has no measurable duration
    /// (e.g. a still image).
    std::chrono::milliseconds duration{0};
    /// Width of the primary video stream in pixels. Zero if no video stream.
    int width{0};
    /// Height of the primary video stream in pixels. Zero if no video stream.
    int height{0};
    /// Average frame rate of the primary video stream (fps).
    double frame_rate{0};
    /// Overall container bit-rate, bits per second.
    std::int64_t bit_rate_bps{0};
    /// Codec name of the primary video stream (e.g. `"h264"`). Empty if none.
    std::string video_codec;
    /// Codec name of the primary audio stream (e.g. `"aac"`). Empty if none.
    std::string audio_codec;
    /// Channel count of the primary audio stream.
    int audio_channels{0};
    /// File size on disk, in bytes (mirrors the container's `format.size`).
    std::int64_t file_size{0};
};

/// Polymorphic media-processor seam. Implementations are wired into the
/// upload pipeline so it can decorate Telegram messages with metadata,
/// thumbnails, samples, and screenshot tiles.
class MediaProcessorInterface {
public:
    virtual ~MediaProcessorInterface() = default;

    MediaProcessorInterface()                                          = default;
    MediaProcessorInterface(const MediaProcessorInterface&)            = delete;
    MediaProcessorInterface& operator=(const MediaProcessorInterface&) = delete;
    MediaProcessorInterface(MediaProcessorInterface&&)                 = delete;
    MediaProcessorInterface& operator=(MediaProcessorInterface&&)      = delete;

    /// Probes @p file with `ffprobe` (or equivalent) and returns a
    /// populated @ref MediaInfo. Returns `MediaProcessing` on failure.
    virtual boost::asio::awaitable<cmlb::core::Result<MediaInfo>>
        probe(std::filesystem::path file) = 0;

    /// Extracts a thumbnail at @p position. When @p position is empty the
    /// implementation should choose a sensible default (CMLB convention:
    /// 10% of the probed duration).
    virtual boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        extract_thumbnail(std::filesystem::path file,
                          std::filesystem::path output,
                          std::optional<std::chrono::seconds> position) = 0;

    /// Produces a short stream-copied sample of length @p duration starting
    /// at offset zero. The output container is inferred from the extension
    /// of @p output (use `.mp4` or `.mkv`).
    virtual boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        generate_sample(std::filesystem::path file,
                        std::filesystem::path output,
                        std::chrono::seconds duration) = 0;

    /// Builds a `@p rows`x`@p columns` screenshot tile evenly distributed
    /// across the duration of the input.
    virtual boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        generate_screenshot_grid(std::filesystem::path file,
                                 std::filesystem::path output,
                                 int rows,
                                 int columns) = 0;
};

}  // namespace cmlb::infrastructure::media
