#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <nlohmann/json.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/media/ffmpeg_media_processor.hpp>
#include <cmlb/infrastructure/system/subprocess.hpp>

namespace cmlb::infrastructure::media {

namespace {

namespace asio = boost::asio;
using cmlb::core::error;
using cmlb::core::ErrorCode;
using cmlb::core::Logger;
using cmlb::core::Result;
using cmlb::infrastructure::system::Subprocess;
using cmlb::infrastructure::system::SubprocessRequest;
using cmlb::infrastructure::system::SubprocessResult;
using json = nlohmann::json;

/// Parses an ffprobe rational frame-rate string like "30000/1001" or "25/1".
/// Returns 0.0 if the input is unparseable or the denominator is zero.
[[nodiscard]] double parse_rational(std::string_view text) noexcept {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) {
        try {
            return std::stod(std::string{text});
        } catch (...) {
            return 0.0;
        }
    }
    try {
        const auto num = std::stod(std::string{text.substr(0, slash)});
        const auto den = std::stod(std::string{text.substr(slash + 1)});
        if (den == 0.0)
            return 0.0;
        return num / den;
    } catch (...) {
        return 0.0;
    }
}

/// Parses an ffprobe duration string ("12.345" seconds) into milliseconds.
[[nodiscard]] std::chrono::milliseconds parse_duration_seconds(std::string_view text) noexcept {
    try {
        const auto secs = std::stod(std::string{text});
        if (secs < 0)
            return std::chrono::milliseconds{0};
        return std::chrono::milliseconds{static_cast<std::int64_t>(secs * 1000.0)};
    } catch (...) {
        return std::chrono::milliseconds{0};
    }
}

/// Parses an integer-valued string; returns 0 on failure.
[[nodiscard]] std::int64_t parse_int64(std::string_view text) noexcept {
    try {
        return std::stoll(std::string{text});
    } catch (...) {
        return 0;
    }
}

/// Formats `seconds` as ffmpeg's `-ss` argument (HH:MM:SS.mmm).
[[nodiscard]] std::string format_timestamp(std::chrono::seconds seconds) {
    auto total = seconds.count();
    if (total < 0)
        total = 0;
    const auto h = total / 3600;
    const auto m = (total % 3600) / 60;
    const auto s = total % 60;
    std::string out;
    out.reserve(11);
    out += std::to_string(h);
    out += ':';
    if (m < 10)
        out += '0';
    out += std::to_string(m);
    out += ':';
    if (s < 10)
        out += '0';
    out += std::to_string(s);
    return out;
}

/// Common subprocess executor: spawns @p req via @p subprocess and validates
/// the exit code. Returns the populated `SubprocessResult` on success.
asio::awaitable<Result<SubprocessResult>> run_subprocess(Subprocess& subprocess,
                                                         SubprocessRequest req,
                                                         ErrorCode failure_code) {
    auto result = co_await subprocess.run(std::move(req));
    if (!result) {
        co_return std::unexpected(result.error());
    }
    if (result->exit_code != 0) {
        Logger::warn(
            "[media] subprocess exited with code {}: {}", result->exit_code, result->stderr_data);
        co_return error(failure_code,
                        "subprocess exited with code " + std::to_string(result->exit_code) + ": "
                            + result->stderr_data);
    }
    co_return std::move(*result);
}

} // namespace

FfmpegMediaProcessor::FfmpegMediaProcessor(Subprocess& subprocess,
                                           std::filesystem::path ffmpeg_path,
                                           std::filesystem::path ffprobe_path)
    : subprocess_{&subprocess},
      ffmpeg_path_{std::move(ffmpeg_path)},
      ffprobe_path_{std::move(ffprobe_path)} {
}

asio::awaitable<Result<MediaInfo>> FfmpegMediaProcessor::probe(std::filesystem::path file) {
    SubprocessRequest req{};
    req.executable = ffprobe_path_;
    req.arguments = {
        "-v",
        "error",
        "-print_format",
        "json",
        "-show_streams",
        "-show_format",
        file.string(),
    };

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::MediaProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    json doc;
    try {
        doc = json::parse(run_result->stdout_data);
    } catch (const json::exception& e) {
        co_return error(ErrorCode::MediaProcessing,
                        std::string{"ffprobe JSON parse failed: "} + e.what());
    }

    MediaInfo info{};

    // ---- format ----------------------------------------------------------
    if (const auto fmt_it = doc.find("format"); fmt_it != doc.end() && fmt_it->is_object()) {
        if (const auto dur_it = fmt_it->find("duration");
            dur_it != fmt_it->end() && dur_it->is_string()) {
            info.duration = parse_duration_seconds(dur_it->get<std::string>());
        }
        if (const auto br_it = fmt_it->find("bit_rate");
            br_it != fmt_it->end() && br_it->is_string()) {
            info.bit_rate_bps = parse_int64(br_it->get<std::string>());
        }
        if (const auto sz_it = fmt_it->find("size"); sz_it != fmt_it->end() && sz_it->is_string()) {
            info.file_size = parse_int64(sz_it->get<std::string>());
        }
    }

    // ---- streams ---------------------------------------------------------
    bool have_video = false;
    bool have_audio = false;
    if (const auto streams_it = doc.find("streams");
        streams_it != doc.end() && streams_it->is_array()) {
        for (const auto& stream : *streams_it) {
            const auto& type_node = stream.value("codec_type", "");
            if (!have_video && type_node == "video") {
                have_video = true;
                info.video_codec = stream.value("codec_name", "");
                info.width = stream.value("width", 0);
                info.height = stream.value("height", 0);
                if (const auto fr_it = stream.find("avg_frame_rate");
                    fr_it != stream.end() && fr_it->is_string()) {
                    info.frame_rate = parse_rational(fr_it->get<std::string>());
                }
                if (info.frame_rate == 0.0) {
                    if (const auto fr_it = stream.find("r_frame_rate");
                        fr_it != stream.end() && fr_it->is_string()) {
                        info.frame_rate = parse_rational(fr_it->get<std::string>());
                    }
                }
            } else if (!have_audio && type_node == "audio") {
                have_audio = true;
                info.audio_codec = stream.value("codec_name", "");
                info.audio_channels = stream.value("channels", 0);
            }
        }
    }

    if (info.file_size == 0) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(file, ec);
        if (!ec)
            info.file_size = static_cast<std::int64_t>(sz);
    }

    co_return info;
}

asio::awaitable<Result<std::filesystem::path>> FfmpegMediaProcessor::extract_thumbnail(
    std::filesystem::path file,
    std::filesystem::path output,
    std::optional<std::chrono::seconds> position) {
    std::chrono::seconds pos{0};
    if (position) {
        pos = *position;
    } else {
        // Probe first to default to 10% of duration.
        auto probe_result = co_await probe(file);
        if (!probe_result) {
            co_return std::unexpected(probe_result.error());
        }
        const auto ms = probe_result->duration.count();
        pos = std::chrono::seconds{ms / 10000}; // 10% in seconds
    }

    SubprocessRequest req{};
    req.executable = ffmpeg_path_;
    req.arguments = {
        "-y",
        "-ss",
        format_timestamp(pos),
        "-i",
        file.string(),
        "-vframes",
        "1",
        "-q:v",
        "2",
        output.string(),
    };

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::MediaProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    co_return output;
}

asio::awaitable<Result<std::filesystem::path>> FfmpegMediaProcessor::generate_sample(
    std::filesystem::path file, std::filesystem::path output, std::chrono::seconds duration) {
    if (duration.count() <= 0) {
        co_return error(ErrorCode::InvalidArgument, "sample duration must be positive");
    }

    SubprocessRequest req{};
    req.executable = ffmpeg_path_;
    req.arguments = {
        "-y",
        "-ss",
        "0",
        "-i",
        file.string(),
        "-t",
        std::to_string(duration.count()),
        "-c",
        "copy",
        output.string(),
    };

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::MediaProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    co_return output;
}

asio::awaitable<Result<std::filesystem::path>> FfmpegMediaProcessor::generate_screenshot_grid(
    std::filesystem::path file, std::filesystem::path output, int rows, int columns) {
    if (rows <= 0 || columns <= 0) {
        co_return error(ErrorCode::InvalidArgument, "grid rows and columns must be positive");
    }

    // Probe to compute the inter-frame interval.
    auto probe_result = co_await probe(file);
    if (!probe_result) {
        co_return std::unexpected(probe_result.error());
    }
    const auto duration_ms = probe_result->duration.count();
    if (duration_ms <= 0) {
        co_return error(ErrorCode::MediaProcessing, "cannot build grid for a zero-duration input");
    }

    const auto tile_count = rows * columns;
    const auto interval_sec = std::max<double>(
        1.0, static_cast<double>(duration_ms) / 1000.0 / static_cast<double>(tile_count));

    // Format interval with up to 3 decimals.
    std::string interval_str;
    {
        char buf[32]{};
        // round to milliseconds
        const auto interval_ms = static_cast<std::int64_t>(interval_sec * 1000.0);
        const auto whole = interval_ms / 1000;
        const auto frac = interval_ms % 1000;
        std::snprintf(buf,
                      sizeof(buf),
                      "%lld.%03lld",
                      static_cast<long long>(whole),
                      static_cast<long long>(frac));
        interval_str = buf;
    }

    const std::string vf = "fps=1/" + interval_str + ",scale=320:180,tile="
                           + std::to_string(columns) + "x" + std::to_string(rows);

    SubprocessRequest req{};
    req.executable = ffmpeg_path_;
    req.arguments = {
        "-y",
        "-i",
        file.string(),
        "-vf",
        vf,
        "-frames:v",
        "1",
        output.string(),
    };

    auto run_result =
        co_await run_subprocess(*subprocess_, std::move(req), ErrorCode::MediaProcessing);
    if (!run_result) {
        co_return std::unexpected(run_result.error());
    }

    co_return output;
}

} // namespace cmlb::infrastructure::media
