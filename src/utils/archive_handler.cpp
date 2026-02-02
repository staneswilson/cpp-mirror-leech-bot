#include "utils/archive_handler.hpp"
#include "core/logger.hpp"

#include <fstream>
#include <algorithm>
#include <array>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cmlb {

// Supported archive extensions
static const std::vector<std::string> kArchiveExtensions = {
    ".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz",
    ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz",
    ".iso", ".cab", ".arj", ".lzh", ".lzma"
};

ArchiveHandler::ArchiveHandler(const Config& config) : config_(config) {
    Logger::info("ArchiveHandler initialized with 7z: {}", config_.sevenz_path);
}

bool ArchiveHandler::isArchive(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    for (const auto& archive_ext : kArchiveExtensions) {
        if (ext == archive_ext) return true;
    }
    
    // Check for .tar.gz style
    std::string stem = path.stem().string();
    if (ext == ".gz" || ext == ".bz2" || ext == ".xz") {
        std::filesystem::path stem_path(stem);
        if (stem_path.extension() == ".tar") return true;
    }
    
    return false;
}

std::string ArchiveHandler::getArchiveType(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext.substr(1); // Remove leading dot
}

Result<std::string> ArchiveHandler::execute7z(const std::vector<std::string>& args) {
    std::string command = config_.sevenz_path;
    for (const auto& arg : args) {
        command += " \"" + arg + "\"";
    }
    
    Logger::debug("Executing: {}", command);

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
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start 7z"));
    }
    
    CloseHandle(hWritePipe);
    
    std::string output;
    std::array<char, 4096> buffer;
    DWORD bytes_read;
    
    while (ReadFile(hReadPipe, buffer.data(), static_cast<DWORD>(buffer.size()), 
                    &bytes_read, NULL) && bytes_read > 0) {
        output.append(buffer.data(), bytes_read);
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    
    if (exit_code != 0) {
        return std::unexpected(AppError(ErrorCode::SystemError, 
            "7z exited with code: " + std::to_string(exit_code)));
    }
    
    return output;
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::unexpected(AppError(ErrorCode::SystemError, "Failed to start 7z"));
    }
    
    std::string output;
    std::array<char, 4096> buffer;
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }
    
    int status = pclose(pipe);
    
    if (WEXITSTATUS(status) != 0) {
        return std::unexpected(AppError(ErrorCode::SystemError, "7z failed"));
    }
    
    return output;
#endif
}

Result<std::vector<std::filesystem::path>> ArchiveHandler::extract(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& dest_path,
    const std::optional<std::string>& password,
    ArchiveProgressCallback progress)
{
    if (!std::filesystem::exists(archive_path)) {
        return std::unexpected(AppError(ErrorCode::FileNotFound, "Archive not found"));
    }
    
    std::filesystem::create_directories(dest_path);
    
    std::vector<std::string> args = {
        "x",                                    // Extract with full paths
        archive_path.string(),
        "-o" + dest_path.string(),              // Output directory
        "-y"                                    // Yes to all prompts
    };
    
    if (password) {
        args.push_back("-p" + *password);
    }
    
    auto result = execute7z(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    // Collect extracted files
    std::vector<std::filesystem::path> extracted;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
        if (entry.is_regular_file()) {
            extracted.push_back(entry.path());
        }
    }
    
    Logger::info("Extracted {} files from {}", extracted.size(), archive_path.filename().string());
    
    if (config_.delete_after_extract) {
        std::filesystem::remove(archive_path);
    }
    
    return extracted;
}

Result<std::vector<std::filesystem::path>> ArchiveHandler::archive(
    const std::filesystem::path& source_path,
    const std::filesystem::path& archive_path,
    const std::optional<std::string>& password,
    int64_t split_size,
    ArchiveProgressCallback progress)
{
    if (!std::filesystem::exists(source_path)) {
        return std::unexpected(AppError(ErrorCode::FileNotFound, "Source not found"));
    }
    
    std::vector<std::string> args = {
        "a",                                    // Add to archive
        archive_path.string(),
        source_path.string()
    };
    
    if (password) {
        args.push_back("-p" + *password);
        args.push_back("-mhe=on");              // Encrypt headers
    }
    
    if (split_size > 0) {
        args.push_back("-v" + std::to_string(split_size / (1024 * 1024)) + "m");
    }
    
    auto result = execute7z(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    // Find created archive(s)
    std::vector<std::filesystem::path> archives;
    
    if (split_size > 0) {
        // Look for split files
        int part = 1;
        while (true) {
            auto part_path = std::filesystem::path(
                archive_path.string() + std::format(".{:03d}", part));
            if (std::filesystem::exists(part_path)) {
                archives.push_back(part_path);
                part++;
            } else {
                break;
            }
        }
    } else {
        archives.push_back(archive_path);
    }
    
    Logger::info("Created {} archive(s) from {}", archives.size(), source_path.filename().string());
    
    if (config_.delete_after_archive && std::filesystem::is_regular_file(source_path)) {
        std::filesystem::remove(source_path);
    }
    
    return archives;
}

Result<std::filesystem::path> ArchiveHandler::joinSplitFiles(
    const std::filesystem::path& first_part,
    const std::filesystem::path& output_path)
{
    // Find all parts
    std::vector<std::filesystem::path> parts;
    
    std::string base = first_part.string();
    // Remove .001 suffix
    if (base.length() > 4 && base.substr(base.length() - 4, 4) == ".001") {
        base = base.substr(0, base.length() - 4);
    }
    
    int part = 1;
    while (true) {
        auto part_path = std::filesystem::path(base + std::format(".{:03d}", part));
        if (std::filesystem::exists(part_path)) {
            parts.push_back(part_path);
            part++;
        } else {
            break;
        }
    }
    
    if (parts.empty()) {
        return std::unexpected(AppError(ErrorCode::FileNotFound, "No split parts found"));
    }
    
    // Join files by concatenating
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        return std::unexpected(AppError(ErrorCode::FileWriteError, "Cannot create output file"));
    }
    
    std::vector<char> buffer(64 * 1024 * 1024); // 64MB buffer
    
    for (const auto& part : parts) {
        std::ifstream input(part, std::ios::binary);
        if (!input) {
            return std::unexpected(AppError(ErrorCode::FileNotFound, "Cannot open part: " + part.string()));
        }
        
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            output.write(buffer.data(), input.gcount());
        }
        
        Logger::debug("Joined part: {}", part.filename().string());
    }
    
    Logger::info("Joined {} parts into {}", parts.size(), output_path.filename().string());
    
    return output_path;
}

Result<std::vector<std::string>> ArchiveHandler::listContents(
    const std::filesystem::path& archive_path,
    const std::optional<std::string>& password)
{
    std::vector<std::string> args = {
        "l",                                    // List
        archive_path.string()
    };
    
    if (password) {
        args.push_back("-p" + *password);
    }
    
    auto result = execute7z(args);
    if (!result) {
        return std::unexpected(result.error());
    }
    
    // Parse output to get file list
    std::vector<std::string> files;
    std::istringstream iss(*result);
    std::string line;
    bool in_list = false;
    
    while (std::getline(iss, line)) {
        if (line.find("---") != std::string::npos) {
            in_list = !in_list;
            continue;
        }
        
        if (in_list && !line.empty()) {
            // Extract filename (last column)
            auto pos = line.rfind(' ');
            if (pos != std::string::npos) {
                files.push_back(line.substr(pos + 1));
            }
        }
    }
    
    return files;
}

} // namespace cmlb
