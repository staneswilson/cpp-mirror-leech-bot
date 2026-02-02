#ifndef CMLB_UTILS_MEDIA_PROCESSOR_HPP
#define CMLB_UTILS_MEDIA_PROCESSOR_HPP

#include "core/types.hpp"
#include <filesystem>
#include <string>
#include <optional>
#include <chrono>

namespace cmlb {

/**
 * @brief Video/audio metadata.
 */
struct MediaInfo {
    std::string format;
    std::string codec;
    int width{0};
    int height{0};
    std::chrono::seconds duration{0};
    int64_t bitrate{0};
    double fps{0.0};
    std::optional<std::string> audio_codec;
    std::optional<int> audio_channels;
};

/**
 * @brief FFmpeg-based media processor.
 * 
 * Features:
 * - Thumbnail extraction
 * - Sample video generation
 * - Format conversion
 * - Media info extraction
 */
class MediaProcessor {
public:
    struct Config {
        std::string ffmpeg_path = "ffmpeg";
        std::string ffprobe_path = "ffprobe";
    };

    explicit MediaProcessor(const Config& config = {});

    /**
     * @brief Check if file is a video.
     */
    static bool isVideo(const std::filesystem::path& path);

    /**
     * @brief Check if file is audio.
     */
    static bool isAudio(const std::filesystem::path& path);

    /**
     * @brief Get media information.
     */
    Result<MediaInfo> getInfo(const std::filesystem::path& path);

    /**
     * @brief Extract thumbnail from video.
     * @param video_path Input video
     * @param output_path Output image path
     * @param time_sec Time position in seconds (default: 10% of duration)
     * @return Thumbnail path
     */
    Result<std::filesystem::path> extractThumbnail(
        const std::filesystem::path& video_path,
        const std::filesystem::path& output_path,
        std::optional<int> time_sec = std::nullopt
    );

    /**
     * @brief Generate sample video (first N seconds).
     * @param video_path Input video
     * @param output_path Output video path
     * @param duration_sec Sample duration (default: 60s)
     * @return Sample video path
     */
    Result<std::filesystem::path> generateSample(
        const std::filesystem::path& video_path,
        const std::filesystem::path& output_path,
        int duration_sec = 60
    );

    /**
     * @brief Generate screenshot grid (multiple frames).
     * @param video_path Input video
     * @param output_path Output image path
     * @param rows Grid rows
     * @param cols Grid columns
     * @return Screenshot grid path
     */
    Result<std::filesystem::path> generateScreenshotGrid(
        const std::filesystem::path& video_path,
        const std::filesystem::path& output_path,
        int rows = 3,
        int cols = 3
    );

    /**
     * @brief Convert video to different format.
     * @param input_path Input video
     * @param output_path Output path (extension determines format)
     * @param extra_args Additional FFmpeg arguments
     * @return Output path
     */
    Result<std::filesystem::path> convert(
        const std::filesystem::path& input_path,
        const std::filesystem::path& output_path,
        const std::string& extra_args = ""
    );

private:
    Config config_;
    
    Result<std::string> executeFFmpeg(const std::vector<std::string>& args);
    Result<std::string> executeFFprobe(const std::vector<std::string>& args);
};

} // namespace cmlb

#endif // CMLB_UTILS_MEDIA_PROCESSOR_HPP
