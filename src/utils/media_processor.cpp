#include "utils/media_processor.hpp"
#include "core/logger.hpp"

#include <array>
#include <algorithm>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cmlb {

using json = nlohmann::json;

static const std::vector<std::string> kVideoExtensions = {
    ".mp4", ".mkv", ".avi", ".webm", ".mov", ".flv", ".wmv", ".m4v", ".mpg", ".mpeg"
};

static const std::vector<std::string> kAudioExtensions = {
    ".mp3", ".flac", ".ogg", ".wav", ".m4a", ".aac", ".wma", ".opus"
};

MediaProcessor::MediaProcessor(const Config& config) : config_(config) {
    Logger::info("MediaProcessor initialized");
}

bool MediaProcessor::isVideo(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return std::find(kVideoExtensions.begin(), kVideoExtensions.end(), ext) != kVideoExtensions.end();
}

bool MediaProcessor::isAudio(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return std::find(kAudioExtensions.begin(), kAudioExtensions.end(), ext) != kAudioExtensions.end();
}

Result<std::string> MediaProcessor::executeFFmpeg(const std::vector<std::string>& args) {
    std::string command = config_.ffmpeg_path;
    for (const auto& arg : args) {
        command += " " + arg;
    }
    
    Logger::debug("Executing: {}", command);

#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    std::string cmd_line = command;
    
    if (!CreateProcessA(NULL, cmd_line.data(), NULL, NULL, FALSE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start FFmpeg"));
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exit_code != 0) {
        return std::unexpected(AppError(ErrorCode::SystemError, "FFmpeg failed"));
    }
    
    return "";
#else
    int result = system(command.c_str());
    if (WEXITSTATUS(result) != 0) {
        return std::unexpected(AppError(ErrorCode::SystemError, "FFmpeg failed"));
    }
    return "";
#endif
}

Result<std::string> MediaProcessor::executeFFprobe(const std::vector<std::string>& args) {
    std::string command = config_.ffprobe_path;
    for (const auto& arg : args) {
        command += " " + arg;
    }
    
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to create pipe"));
    }
    
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));
    
    std::string cmd_line = command;
    
    if (!CreateProcessA(NULL, cmd_line.data(), NULL, NULL, TRUE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start FFprobe"));
    }
    
    CloseHandle(hWritePipe);
    
    std::string output;
    std::array<char, 4096> buffer;
    DWORD bytes_read;
    
    while (ReadFile(hReadPipe, buffer.data(), static_cast<DWORD>(buffer.size()), 
                    &bytes_read, NULL) && bytes_read > 0) {
        output.append(buffer.data(), bytes_read);
    }
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    
    return output;
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start FFprobe"));
    }
    
    std::string output;
    std::array<char, 4096> buffer;
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    
    pclose(pipe);
    return output;
#endif
}

Result<MediaInfo> MediaProcessor::getInfo(const std::filesystem::path& path) {
    std::vector<std::string> args = {
        "-v", "quiet",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        "\"" + path.string() + "\""
    };
    
    auto result = executeFFprobe(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    try {
        json j = json::parse(*result);
        MediaInfo info;
        
        // Parse format
        if (j.contains("format")) {
            info.format = j["format"].value("format_name", "");
            info.bitrate = std::stoll(j["format"].value("bit_rate", "0"));
            double dur = std::stod(j["format"].value("duration", "0"));
            info.duration = std::chrono::seconds(static_cast<int>(dur));
        }
        
        // Parse streams
        if (j.contains("streams")) {
            for (const auto& stream : j["streams"]) {
                std::string type = stream.value("codec_type", "");
                
                if (type == "video") {
                    info.codec = stream.value("codec_name", "");
                    info.width = stream.value("width", 0);
                    info.height = stream.value("height", 0);
                    
                    std::string fps_str = stream.value("r_frame_rate", "0/1");
                    auto slash = fps_str.find('/');
                    if (slash != std::string::npos) {
                        double num = std::stod(fps_str.substr(0, slash));
                        double den = std::stod(fps_str.substr(slash + 1));
                        if (den > 0) info.fps = num / den;
                    }
                } else if (type == "audio") {
                    info.audio_codec = stream.value("codec_name", "");
                    info.audio_channels = stream.value("channels", 0);
                }
            }
        }
        
        return info;
        
    } catch (const json::exception& e) {
        return std::unexpected(AppError(ErrorCode::JsonParseError, e.what()));
    }
}

Result<std::filesystem::path> MediaProcessor::extractThumbnail(
    const std::filesystem::path& video_path,
    const std::filesystem::path& output_path,
    std::optional<int> time_sec)
{
    // Get video duration if time not specified
    int seek_time = 10;
    if (!time_sec) {
        auto info = getInfo(video_path);
        if (info) {
            seek_time = static_cast<int>(info->duration.count() * 0.1); // 10% in
        }
    } else {
        seek_time = *time_sec;
    }
    
    std::vector<std::string> args = {
        "-y",
        "-ss", std::to_string(seek_time),
        "-i", "\"" + video_path.string() + "\"",
        "-vframes", "1",
        "-q:v", "2",
        "\"" + output_path.string() + "\""
    };
    
    auto result = executeFFmpeg(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    if (!std::filesystem::exists(output_path)) {
        return std::unexpected(AppError(ErrorCode::FileWriteError, "Thumbnail not created"));
    }
    
    Logger::info("Extracted thumbnail: {}", output_path.filename().string());
    return output_path;
}

Result<std::filesystem::path> MediaProcessor::generateSample(
    const std::filesystem::path& video_path,
    const std::filesystem::path& output_path,
    int duration_sec)
{
    std::vector<std::string> args = {
        "-y",
        "-i", "\"" + video_path.string() + "\"",
        "-t", std::to_string(duration_sec),
        "-c", "copy",
        "\"" + output_path.string() + "\""
    };
    
    auto result = executeFFmpeg(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    Logger::info("Generated sample video: {}", output_path.filename().string());
    return output_path;
}

Result<std::filesystem::path> MediaProcessor::generateScreenshotGrid(
    const std::filesystem::path& video_path,
    const std::filesystem::path& output_path,
    int rows,
    int cols)
{
    auto info = getInfo(video_path);
    if (!info) {
        return std::unexpected(info.error());
    }
    
    int total_frames = rows * cols;
    int interval = static_cast<int>(info->duration.count()) / (total_frames + 1);
    
    // Generate filter for tile grid
    std::string filter = std::format(
        "select='not(mod(n\\,{}))',scale=320:-1,tile={}x{}",
        static_cast<int>(info->fps * interval),
        cols,
        rows
    );
    
    std::vector<std::string> args = {
        "-y",
        "-i", "\"" + video_path.string() + "\"",
        "-vf", "\"" + filter + "\"",
        "-frames:v", "1",
        "\"" + output_path.string() + "\""
    };
    
    auto result = executeFFmpeg(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    Logger::info("Generated screenshot grid: {}", output_path.filename().string());
    return output_path;
}

Result<std::filesystem::path> MediaProcessor::convert(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path,
    const std::string& extra_args)
{
    std::vector<std::string> args = {
        "-y",
        "-i", "\"" + input_path.string() + "\""
    };
    
    if (!extra_args.empty()) {
        args.push_back(extra_args);
    }
    
    args.push_back("\"" + output_path.string() + "\"");
    
    auto result = executeFFmpeg(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    Logger::info("Converted: {} -> {}", input_path.filename().string(), output_path.filename().string());
    return output_path;
}

} // namespace cmlb
