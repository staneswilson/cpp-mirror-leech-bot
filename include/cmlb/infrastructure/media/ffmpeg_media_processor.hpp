#pragma once

#include <chrono>
#include <filesystem>
#include <optional>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/media/media_processor_interface.hpp>

namespace cmlb::infrastructure::system {
class Subprocess;
}  // namespace cmlb::infrastructure::system

namespace cmlb::infrastructure::media {

/// `MediaProcessorInterface` adapter backed by the `ffmpeg` / `ffprobe`
/// command-line tools. All process invocations are routed through the
/// injected `system::Subprocess` so no direct `popen`/`std::system` calls
/// occur in this layer.
class FfmpegMediaProcessor final : public MediaProcessorInterface {
public:
    /// Constructs the processor.
    /// @param subprocess  Subprocess gateway used to spawn ffmpeg/ffprobe.
    /// @param ffmpeg_path  Resolved path or PATH-relative name of ffmpeg.
    /// @param ffprobe_path Resolved path or PATH-relative name of ffprobe.
    explicit FfmpegMediaProcessor(cmlb::infrastructure::system::Subprocess& subprocess,
                                  std::filesystem::path ffmpeg_path  = "ffmpeg",
                                  std::filesystem::path ffprobe_path = "ffprobe");

    ~FfmpegMediaProcessor() override = default;

    FfmpegMediaProcessor(const FfmpegMediaProcessor&)            = delete;
    FfmpegMediaProcessor& operator=(const FfmpegMediaProcessor&) = delete;
    FfmpegMediaProcessor(FfmpegMediaProcessor&&)                 = delete;
    FfmpegMediaProcessor& operator=(FfmpegMediaProcessor&&)      = delete;

    boost::asio::awaitable<cmlb::core::Result<MediaInfo>>
        probe(std::filesystem::path file) override;

    boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        extract_thumbnail(std::filesystem::path file,
                          std::filesystem::path output,
                          std::optional<std::chrono::seconds> position) override;

    boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        generate_sample(std::filesystem::path file,
                        std::filesystem::path output,
                        std::chrono::seconds duration) override;

    boost::asio::awaitable<cmlb::core::Result<std::filesystem::path>>
        generate_screenshot_grid(std::filesystem::path file,
                                 std::filesystem::path output,
                                 int rows,
                                 int columns) override;

    /// Exposes the configured ffmpeg path (for diagnostics/tests).
    [[nodiscard]] const std::filesystem::path& ffmpeg_path() const noexcept {
        return ffmpeg_path_;
    }

    /// Exposes the configured ffprobe path (for diagnostics/tests).
    [[nodiscard]] const std::filesystem::path& ffprobe_path() const noexcept {
        return ffprobe_path_;
    }

private:
    cmlb::infrastructure::system::Subprocess* subprocess_;
    std::filesystem::path                     ffmpeg_path_;
    std::filesystem::path                     ffprobe_path_;
};

}  // namespace cmlb::infrastructure::media
