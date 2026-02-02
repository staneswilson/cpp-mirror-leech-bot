#include "uploaders/telegram_uploader.hpp"
#include "core/logger.hpp"

#include <td/telegram/td_api.h>
#include <fstream>
#include <algorithm>
#include <thread>

namespace cmlb {

// File extension to content type mapping
static const std::unordered_map<std::string, TelegramUploader::ContentType> kExtensionMap = {
    // Photos
    {".jpg", TelegramUploader::ContentType::Photo},
    {".jpeg", TelegramUploader::ContentType::Photo},
    {".png", TelegramUploader::ContentType::Photo},
    {".webp", TelegramUploader::ContentType::Photo},
    // Videos
    {".mp4", TelegramUploader::ContentType::Video},
    {".mkv", TelegramUploader::ContentType::Video},
    {".avi", TelegramUploader::ContentType::Video},
    {".webm", TelegramUploader::ContentType::Video},
    {".mov", TelegramUploader::ContentType::Video},
    // Audio
    {".mp3", TelegramUploader::ContentType::Audio},
    {".flac", TelegramUploader::ContentType::Audio},
    {".ogg", TelegramUploader::ContentType::Audio},
    {".wav", TelegramUploader::ContentType::Audio},
    {".m4a", TelegramUploader::ContentType::Audio},
};

TelegramUploader::TelegramUploader(
    std::shared_ptr<td::ClientManager> client_manager,
    std::int32_t client_id)
    : client_manager_(std::move(client_manager))
    , client_id_(client_id)
{
    Logger::info("TelegramUploader initialized");
}

TelegramUploader::~TelegramUploader() = default;

TelegramUploader::ContentType TelegramUploader::detectContentType(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    auto it = kExtensionMap.find(ext);
    if (it != kExtensionMap.end()) {
        return it->second;
    }
    return ContentType::Document;
}

std::future<Result<UploadResult>> TelegramUploader::uploadFile(
    const std::filesystem::path& path,
    const UploadConfig& config,
    UploadProgressCallback progress)
{
    return std::async(std::launch::async, [this, path, config, progress]() -> Result<UploadResult> {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(AppError(ErrorCode::FileNotFound, "File not found: " + path.string()));
        }
        
        if (!config.chat_id) {
            return std::unexpected(AppError(ErrorCode::InvalidArgument, "No chat_id specified"));
        }
        
        auto file_size = std::filesystem::file_size(path);
        auto start_time = std::chrono::steady_clock::now();
        
        // Check if splitting is needed
        if (file_size > config.split_size) {
            Logger::info("File {} exceeds split size, splitting into parts", path.filename().string());
            
            auto split_result = splitFile(path, config.split_size);
            if (!split_result) {
                return std::unexpected(split_result.error());
            }
            
            // Upload each part
            UploadResult final_result;
            final_result.success = true;
            
            for (size_t i = 0; i < split_result->size(); ++i) {
                const auto& part_path = (*split_result)[i];
                
                if (progress) {
                    UploadProgress prog;
                    prog.file_name = path.filename().string();
                    prog.current_part = static_cast<int>(i + 1);
                    prog.total_parts = static_cast<int>(split_result->size());
                    progress(prog);
                }
                
                // Create input file
                auto input_file = td::td_api::make_object<td::td_api::inputFileLocal>();
                input_file->path_ = part_path.string();
                
                // Create message content (always document for split files)
                auto content = td::td_api::make_object<td::td_api::inputMessageDocument>();
                content->document_ = std::move(input_file);
                
                auto caption = td::td_api::make_object<td::td_api::formattedText>();
                caption->text_ = std::format("Part {}/{}: {}", i + 1, split_result->size(), path.filename().string());
                content->caption_ = std::move(caption);
                
                // Send message
                auto request = td::td_api::make_object<td::td_api::sendMessage>();
                request->chat_id_ = *config.chat_id;
                request->input_message_content_ = std::move(content);
                
                client_manager_->send(client_id_, 0, std::move(request));
                
                // Small delay between parts
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            // Cleanup split files
            for (const auto& part : *split_result) {
                std::filesystem::remove(part);
            }
            
            auto end_time = std::chrono::steady_clock::now();
            final_result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            final_result.size = file_size;
            
            return final_result;
        }
        
        // Single file upload
        auto content_type = config.as_document ? ContentType::Document : detectContentType(path);
        
        auto input_file = td::td_api::make_object<td::td_api::inputFileLocal>();
        input_file->path_ = path.string();
        
        std::unique_ptr<td::td_api::InputMessageContent> content;
        
        switch (content_type) {
            case ContentType::Photo: {
                auto photo = td::td_api::make_object<td::td_api::inputMessagePhoto>();
                photo->photo_ = std::move(input_file);
                if (config.caption) {
                    photo->caption_ = td::td_api::make_object<td::td_api::formattedText>();
                    photo->caption_->text_ = *config.caption;
                }
                content = std::move(photo);
                break;
            }
            case ContentType::Video: {
                auto video = td::td_api::make_object<td::td_api::inputMessageVideo>();
                video->video_ = std::move(input_file);
                if (config.caption) {
                    video->caption_ = td::td_api::make_object<td::td_api::formattedText>();
                    video->caption_->text_ = *config.caption;
                }
                if (config.thumbnail_path) {
                    auto thumb = td::td_api::make_object<td::td_api::inputThumbnail>();
                    thumb->thumbnail_ = td::td_api::make_object<td::td_api::inputFileLocal>();
                    static_cast<td::td_api::inputFileLocal*>(thumb->thumbnail_.get())->path_ = *config.thumbnail_path;
                    video->thumbnail_ = std::move(thumb);
                }
                content = std::move(video);
                break;
            }
            case ContentType::Audio: {
                auto audio = td::td_api::make_object<td::td_api::inputMessageAudio>();
                audio->audio_ = std::move(input_file);
                if (config.caption) {
                    audio->caption_ = td::td_api::make_object<td::td_api::formattedText>();
                    audio->caption_->text_ = *config.caption;
                }
                content = std::move(audio);
                break;
            }
            default: {
                auto doc = td::td_api::make_object<td::td_api::inputMessageDocument>();
                doc->document_ = std::move(input_file);
                if (config.caption) {
                    doc->caption_ = td::td_api::make_object<td::td_api::formattedText>();
                    doc->caption_->text_ = *config.caption;
                }
                content = std::move(doc);
                break;
            }
        }
        
        auto request = td::td_api::make_object<td::td_api::sendMessage>();
        request->chat_id_ = *config.chat_id;
        request->input_message_content_ = std::move(content);
        
        client_manager_->send(client_id_, 0, std::move(request));
        
        auto end_time = std::chrono::steady_clock::now();
        
        UploadResult result;
        result.success = true;
        result.size = file_size;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        Logger::info("Uploaded {} ({} bytes)", path.filename().string(), file_size);
        
        return result;
    });
}

std::future<Result<std::vector<UploadResult>>> TelegramUploader::uploadDirectory(
    const std::filesystem::path& path,
    const UploadConfig& config,
    UploadProgressCallback progress)
{
    return std::async(std::launch::async, [this, path, config, progress]() -> Result<std::vector<UploadResult>> {
        if (!std::filesystem::is_directory(path)) {
            return std::unexpected(AppError(ErrorCode::InvalidArgument, "Not a directory: " + path.string()));
        }
        
        std::vector<UploadResult> results;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                auto result = uploadFile(entry.path(), config, progress).get();
                if (result) {
                    results.push_back(*result);
                }
            }
        }
        
        return results;
    });
}

void TelegramUploader::cancelUpload(std::string_view /*upload_id*/) {
    // Tdlib doesn't support canceling individual uploads easily
    // Would need to track message IDs and delete them
    Logger::warn("Upload cancellation not fully implemented");
}

Result<std::vector<std::filesystem::path>> TelegramUploader::splitFile(
    const std::filesystem::path& path,
    int64_t part_size)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(AppError(ErrorCode::FileNotFound, "Cannot open file"));
    }
    
    auto file_size = std::filesystem::file_size(path);
    int num_parts = static_cast<int>((file_size + part_size - 1) / part_size);
    
    std::vector<std::filesystem::path> parts;
    std::vector<char> buffer(static_cast<size_t>(std::min(part_size, static_cast<int64_t>(64 * 1024 * 1024)))); // 64MB buffer max
    
    for (int i = 0; i < num_parts; ++i) {
        std::filesystem::path part_path = path;
        part_path += std::format(".part{:03d}", i + 1);
        
        std::ofstream output(part_path, std::ios::binary);
        if (!output) {
            // Cleanup already created parts
            for (const auto& p : parts) {
                std::filesystem::remove(p);
            }
            return std::unexpected(AppError(ErrorCode::FileWriteError, "Cannot create part file"));
        }
        
        int64_t bytes_remaining = part_size;
        while (bytes_remaining > 0 && input) {
            auto to_read = static_cast<std::streamsize>(std::min(bytes_remaining, static_cast<int64_t>(buffer.size())));
            input.read(buffer.data(), to_read);
            auto bytes_read = input.gcount();
            if (bytes_read > 0) {
                output.write(buffer.data(), bytes_read);
                bytes_remaining -= bytes_read;
            }
            if (bytes_read < to_read) break;
        }
        
        parts.push_back(part_path);
        Logger::info("Created part {}/{}: {}", i + 1, num_parts, part_path.filename().string());
    }
    
    return parts;
}

} // namespace cmlb
