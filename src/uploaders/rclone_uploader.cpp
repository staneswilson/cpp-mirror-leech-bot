#include "uploaders/rclone_uploader.hpp"
#include "core/logger.hpp"

#include <array>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace cmlb {

RcloneUploader::RcloneUploader(const Config& config) : config_(config) {
    Logger::info("RcloneUploader initialized with: {}", config_.rclone_path);
}

RcloneUploader::~RcloneUploader() {
    cancelled_.store(true);
}

bool RcloneUploader::isReady() const noexcept {
    // Check if rclone is available
    // A simple check would be to run "rclone version"
    return true; // Simplified for now
}

std::future<Result<UploadResult>> RcloneUploader::uploadFile(
    const std::filesystem::path& path,
    const UploadConfig& config,
    UploadProgressCallback progress)
{
    return std::async(std::launch::async, [this, path, config, progress]() -> Result<UploadResult> {
        if (!config.rclone_path) {
            return std::unexpected(AppError(ErrorCode::InvalidArgument, "No rclone destination specified"));
        }
        
        if (!std::filesystem::exists(path)) {
            return std::unexpected(AppError(ErrorCode::FileNotFound, "File not found: " + path.string()));
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<std::string> args = {
            "copyto",
            path.string(),
            *config.rclone_path + "/" + path.filename().string(),
            "--progress",
            "-v"
        };
        
        if (!config_.config_path.empty()) {
            args.push_back("--config");
            args.push_back(config_.config_path);
        }
        
        args.push_back("--transfers");
        args.push_back(std::to_string(config_.transfers));
        
        auto result = executeRclone(args, progress);
        
        auto end_time = std::chrono::steady_clock::now();
        
        UploadResult upload_result;
        upload_result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        upload_result.size = std::filesystem::file_size(path);
        
        if (result) {
            upload_result.success = true;
            upload_result.link = *config.rclone_path + "/" + path.filename().string();
            Logger::info("Rclone upload complete: {}", path.filename().string());
        } else {
            upload_result.success = false;
            upload_result.error_message = result.error().message;
            Logger::error("Rclone upload failed: {}", result.error().message);
        }
        
        return upload_result;
    });
}

std::future<Result<std::vector<UploadResult>>> RcloneUploader::uploadDirectory(
    const std::filesystem::path& path,
    const UploadConfig& config,
    UploadProgressCallback progress)
{
    return std::async(std::launch::async, [this, path, config, progress]() -> Result<std::vector<UploadResult>> {
        if (!config.rclone_path) {
            return std::unexpected(AppError(ErrorCode::InvalidArgument, "No rclone destination specified"));
        }
        
        if (!std::filesystem::is_directory(path)) {
            return std::unexpected(AppError(ErrorCode::InvalidArgument, "Not a directory"));
        }
        
        std::vector<std::string> args = {
            "copy",
            path.string(),
            *config.rclone_path + "/" + path.filename().string(),
            "--progress",
            "-v"
        };
        
        if (!config_.config_path.empty()) {
            args.push_back("--config");
            args.push_back(config_.config_path);
        }
        
        auto result = executeRclone(args, progress);
        
        std::vector<UploadResult> results;
        UploadResult dir_result;
        
        if (result) {
            dir_result.success = true;
            dir_result.link = *config.rclone_path + "/" + path.filename().string();
        } else {
            dir_result.success = false;
            dir_result.error_message = result.error().message;
        }
        
        results.push_back(dir_result);
        return results;
    });
}

void RcloneUploader::cancelUpload(std::string_view /*upload_id*/) {
    cancelled_.store(true);
    Logger::info("Rclone upload cancellation requested");
}

Result<std::vector<std::string>> RcloneUploader::listRemotes() {
    auto result = executeRclone({"listremotes"});
    
    if (!result) {
        return std::unexpected(result.error());
    }
    
    std::vector<std::string> remotes;
    std::istringstream iss(*result);
    std::string line;
    
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            // Remove trailing colon
            if (line.back() == ':') {
                line.pop_back();
            }
            remotes.push_back(line);
        }
    }
    
    return remotes;
}

UploadProgress RcloneUploader::parseProgress(const std::string& line) {
    UploadProgress progress;
    
    // Parse rclone progress format:
    // Transferred:   	   1.234 GBytes / 10.000 GBytes, 12%, 50.000 MBytes/s, ETA 2m30s
    std::regex progress_regex(R"(Transferred:\s+[\d.]+ \w+ / ([\d.]+) (\w+), (\d+)%, ([\d.]+) (\w+)/s, ETA (.+))");
    std::smatch match;
    
    if (std::regex_search(line, match, progress_regex)) {
        // Parse percentage
        progress.uploaded_bytes = std::stoll(match[3].str());
        progress.total_bytes = 100; // Using percentage as proxy
        
        // Parse speed
        double speed_val = std::stod(match[4].str());
        std::string speed_unit = match[5].str();
        if (speed_unit == "MBytes") {
            progress.speed_bps = static_cast<int64_t>(speed_val * 1024 * 1024);
        } else if (speed_unit == "GBytes") {
            progress.speed_bps = static_cast<int64_t>(speed_val * 1024 * 1024 * 1024);
        } else {
            progress.speed_bps = static_cast<int64_t>(speed_val);
        }
    }
    
    return progress;
}

Result<std::string> RcloneUploader::executeRclone(
    const std::vector<std::string>& args,
    UploadProgressCallback progress)
{
    std::string command = config_.rclone_path;
    for (const auto& arg : args) {
        command += " \"" + arg + "\"";
    }
    
    Logger::debug("Executing: {}", command);
    
#ifdef _WIN32
    // Windows implementation using CreateProcess
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
    
    if (!CreateProcessA(NULL, cmd_line.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start rclone"));
    }
    
    CloseHandle(hWritePipe);
    
    std::string output;
    std::array<char, 4096> buffer;
    DWORD bytes_read;
    
    while (ReadFile(hReadPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, NULL) && bytes_read > 0) {
        output.append(buffer.data(), bytes_read);
        
        if (progress) {
            // Parse and report progress
            auto prog = parseProgress(std::string(buffer.data(), bytes_read));
            if (prog.total_bytes > 0) {
                progress(prog);
            }
        }
        
        if (cancelled_.load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    
    if (exit_code != 0 && !cancelled_.load()) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Rclone exited with code: " + std::to_string(exit_code)));
    }
    
    return output;
#else
    // Unix implementation using popen
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start rclone"));
    }
    
    std::string output;
    std::array<char, 4096> buffer;
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
        
        if (progress) {
            auto prog = parseProgress(buffer.data());
            if (prog.total_bytes > 0) {
                progress(prog);
            }
        }
        
        if (cancelled_.load()) {
            break;
        }
    }
    
    int status = pclose(pipe);
    
    if (WEXITSTATUS(status) != 0 && !cancelled_.load()) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Rclone failed"));
    }
    
    return output;
#endif
}

} // namespace cmlb
