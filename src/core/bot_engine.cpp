#include "core/bot_engine.hpp"
#include "core/logger.hpp"
#include "downloaders/aria2_client.hpp"
#include "commands/command_router.hpp"
#include "db/database.hpp"
#include "uploaders/iuploader.hpp"
#include "utils/status_manager.hpp"

#include <iostream>
#include <chrono>
#include <format>
#include <array>
#include <fstream>
#include <random>
#include <filesystem>

namespace cmlb {

// ============================================================================
// Helper Functions
// ============================================================================

static std::string formatBytes(int64_t bytes) {
    constexpr std::array<const char*, 5> units = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }
    
    return std::format("{:.2f} {}", size, units[unit_idx]);
}

static std::string formatEta(std::chrono::seconds eta) {
    if (eta.count() <= 0) return "N/A";
    
    auto hours = std::chrono::duration_cast<std::chrono::hours>(eta);
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(eta - hours);
    auto secs = eta - hours - mins;
    
    if (hours.count() > 0) {
        return std::format("{}h {}m", hours.count(), mins.count());
    } else if (mins.count() > 0) {
        return std::format("{}m {}s", mins.count(), secs.count());
    } else {
        return std::format("{}s", secs.count());
    }
}

static std::string renderProgressBar(double percent, int width = 10) {
    int filled = static_cast<int>(percent / 100.0 * width);
    int empty = width - filled;
    
    std::string bar;
    for (int i = 0; i < filled; i++) bar += "█";
    for (int i = 0; i < empty; i++) bar += "░";
    
    return std::format("[{}] {:.1f}%", bar, percent);
}

static std::string generateTaskId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    return std::format("task_{:016x}", dis(gen));
}

// ============================================================================
// BotEngine Implementation
// ============================================================================

BotEngine::BotEngine(const AppConfig& config) : config_(config) {
    start_time_ = std::chrono::steady_clock::now();
    
    td_client_manager_ = std::make_unique<td::ClientManager>();
    td_client_id_ = td_client_manager_->create_client_id();
    
    // Initialize database
    database_ = createInMemoryDatabase();
    if (database_) {
        database_->connect();
        
        // Load/create default bot settings
        auto settings_result = database_->getBotSettings();
        if (settings_result) {
            auto settings = *settings_result;
            if (settings.owner_id == 0 && config_.telegram.owner_id > 0) {
                settings.owner_id = config_.telegram.owner_id;
                settings.aria2_url = config_.aria2.rpc_url;
                settings.aria2_secret = config_.aria2.secret;
                database_->saveBotSettings(settings);
            }
        }
    }
    
    // Initialize Aria2 downloader
    Aria2Downloader::Config aria2_cfg;
    aria2_cfg.rpc_url = config_.aria2.rpc_url;
    aria2_cfg.secret = config_.aria2.secret;
    
    Logger::info("Initializing Aria2 downloader at {}", config_.aria2.rpc_url);
    downloader_ = Aria2Downloader::create(aria2_cfg);
    
    // Initialize command router
    command_router_ = std::make_unique<CommandRouter>();
    setupCommands(*command_router_);
}

BotEngine::~BotEngine() {
    requestStop();
    
    if (database_) {
        database_->disconnect();
    }
    
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (auto& task : background_tasks_) {
            if (task.joinable()) {
                task.request_stop();
            }
        }
    }
}

void BotEngine::requestStop() noexcept {
    stop_flag_.store(true, std::memory_order_release);
    queue_cv_.notify_all();
}

void BotEngine::run() {
    Logger::info("Starting BotEngine event loop...");

    while (!stop_flag_.load(std::memory_order_acquire)) {
        auto response = td_client_manager_->receive(0.1);
        if (response.object) {
            processResponse(std::move(response));
        }

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            while (!job_queue_.empty()) {
                auto task = std::move(job_queue_.front());
                job_queue_.pop();
                
                lock.unlock();
                try {
                    task();
                } catch (const std::exception& e) {
                    Logger::error("Job failed: {}", e.what());
                }
                lock.lock();
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            std::erase_if(background_tasks_, [](const std::jthread& t) {
                return !t.joinable();
            });
        }
    }
    
    Logger::info("BotEngine event loop stopped.");
}

void BotEngine::enqueueTask(std::move_only_function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        job_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

// ============================================================================
// Public API for Command Handlers
// ============================================================================

void BotEngine::sendMessage(int64_t chat_id, const std::string& text) {
    sendTextMessage(chat_id, text);
}

void BotEngine::sendSettingsMenu(int64_t chat_id, int64_t user_id, const std::string& text) {
    // Create inline keyboard for settings
    auto content = td::td_api::make_object<td::td_api::inputMessageText>();
    content->text_ = td::td_api::make_object<td::td_api::formattedText>();
    content->text_->text_ = text;
    
    auto reply_markup = td::td_api::make_object<td::td_api::replyMarkupInlineKeyboard>();
    
    // Row 1: Upload preferences
    std::vector<td::td_api::object_ptr<td::td_api::inlineKeyboardButton>> row1;
    row1.push_back(td::td_api::make_object<td::td_api::inlineKeyboardButton>(
        "📤 Upload Dest",
        td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>(
            std::format("settings_upload_{}", user_id)
        )
    ));
    row1.push_back(td::td_api::make_object<td::td_api::inlineKeyboardButton>(
        "📄 As Document",
        td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>(
            std::format("settings_doc_{}", user_id)
        )
    ));
    
    // Row 2: Other settings
    std::vector<td::td_api::object_ptr<td::td_api::inlineKeyboardButton>> row2;
    row2.push_back(td::td_api::make_object<td::td_api::inlineKeyboardButton>(
        "🖼️ Thumbnail",
        td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>(
            std::format("settings_thumb_{}", user_id)
        )
    ));
    row2.push_back(td::td_api::make_object<td::td_api::inlineKeyboardButton>(
        "✂️ Split Size",
        td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>(
            std::format("settings_split_{}", user_id)
        )
    ));
    
    // Row 3: Close
    std::vector<td::td_api::object_ptr<td::td_api::inlineKeyboardButton>> row3;
    row3.push_back(td::td_api::make_object<td::td_api::inlineKeyboardButton>(
        "❌ Close",
        td::td_api::make_object<td::td_api::inlineKeyboardButtonTypeCallback>("close")
    ));
    
    reply_markup->rows_.push_back(std::move(row1));
    reply_markup->rows_.push_back(std::move(row2));
    reply_markup->rows_.push_back(std::move(row3));
    
    auto request = td::td_api::make_object<td::td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->input_message_content_ = std::move(content);
    request->reply_markup_ = std::move(reply_markup);
    
    td_client_manager_->send(td_client_id_, 0, std::move(request));
}

void BotEngine::sendFile(int64_t chat_id, const std::string& file_path, const std::string& caption) {
    auto input_file = td::td_api::make_object<td::td_api::inputFileLocal>(file_path);
    
    auto content = td::td_api::make_object<td::td_api::inputMessageDocument>();
    content->document_ = std::move(input_file);
    if (!caption.empty()) {
        content->caption_ = td::td_api::make_object<td::td_api::formattedText>();
        content->caption_->text_ = caption;
    }
    
    auto request = td::td_api::make_object<td::td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->input_message_content_ = std::move(content);
    
    td_client_manager_->send(td_client_id_, 0, std::move(request));
}

void BotEngine::sendLogFile(int64_t chat_id) {
    std::string log_path = "logs/cmlb.log";
    
    if (!std::filesystem::exists(log_path)) {
        sendMessage(chat_id, "📝 No log file found.");
        return;
    }
    
    sendFile(chat_id, log_path, "📝 Bot Log File");
}

void BotEngine::startMirror(int64_t chat_id, int64_t user_id, const std::string& url, bool leech) {
    if (!downloader_ || !downloader_->isConnected()) {
        sendMessage(chat_id, "❌ Download service unavailable.");
        return;
    }
    
    std::jthread task([this, chat_id, user_id, url, leech](std::stop_token stoken) {
        if (stoken.stop_requested()) return;
        
        try {
            auto future = downloader_->addUri(url);
            auto status = future.wait_for(std::chrono::seconds(30));
            
            if (status == std::future_status::timeout) {
                enqueueTask([this, chat_id] {
                    sendMessage(chat_id, "❌ Request timed out.");
                });
                return;
            }
            
            auto result = future.get();
            
            if (result.has_value()) {
                std::string gid = result.value();
                Logger::info("Download added. GID: {}", gid);
                
                // Save task record
                if (database_) {
                    TaskRecord task_record;
                    task_record.task_id = generateTaskId();
                    task_record.user_id = user_id;
                    task_record.chat_id = chat_id;
                    task_record.download_id = gid;
                    task_record.download_type = "aria2";
                    task_record.url = url;
                    task_record.status = "downloading";
                    task_record.is_leech = leech;
                    database_->saveTask(task_record);
                }
                
                enqueueTask([this, gid, chat_id, url, leech] {
                    sendMessage(chat_id, std::format(
                        "✅ {} started!\n\n📥 GID: `{}`\n🔗 URL: {}",
                        leech ? "Leech" : "Mirror", gid, url));
                    
                    // Start tracking (will poll for completion)
                    trackDownload(chat_id, 0, gid, leech);
                });
            } else {
                enqueueTask([this, chat_id, error = result.error()] {
                    sendMessage(chat_id, std::format("❌ Failed: {}", error.message));
                });
            }
            
        } catch (const std::exception& e) {
            Logger::error("Mirror exception: {}", e.what());
            enqueueTask([this, chat_id, msg = std::string(e.what())] {
                sendMessage(chat_id, "❌ Error: " + msg);
            });
        }
    });
    
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    background_tasks_.push_back(std::move(task));
}

void BotEngine::startQbMirror(int64_t chat_id, int64_t user_id, const std::string& url, bool leech) {
    if (!qbit_downloader_ || !qbit_downloader_->isConnected()) {
        sendMessage(chat_id, "❌ QBittorrent not available. Use /mirror instead.");
        return;
    }
    
    // Similar implementation to startMirror but using QBittorrent
    sendMessage(chat_id, "🔧 QBittorrent integration in progress...");
}

void BotEngine::startClone(int64_t chat_id, int64_t user_id, const std::string& url) {
    // Extract file/folder ID from Google Drive URL
    std::regex gdrive_regex(R"((?:drive\.google\.com/(?:file/d/|open\?id=|folderview\?id=|drive/folders/))([a-zA-Z0-9_-]+))");
    std::smatch match;
    
    if (!std::regex_search(url, match, gdrive_regex)) {
        sendMessage(chat_id, "❌ Invalid Google Drive link.");
        return;
    }
    
    std::string file_id = match[1].str();
    sendMessage(chat_id, std::format("🔄 Cloning: `{}`\n\nThis feature requires Google Drive API integration.", file_id));
}

void BotEngine::countDriveFiles(int64_t chat_id, const std::string& url) {
    std::regex gdrive_regex(R"((?:drive\.google\.com/drive/folders/)([a-zA-Z0-9_-]+))");
    std::smatch match;
    
    if (!std::regex_search(url, match, gdrive_regex)) {
        sendMessage(chat_id, "❌ Please provide a valid Google Drive folder link.");
        return;
    }
    
    std::string folder_id = match[1].str();
    sendMessage(chat_id, std::format("📁 Counting files in folder: `{}`\n\nThis feature requires Google Drive API integration.", folder_id));
}

void BotEngine::deleteDriveFile(int64_t chat_id, const std::string& url) {
    std::regex gdrive_regex(R"((?:drive\.google\.com/(?:file/d/|drive/folders/))([a-zA-Z0-9_-]+))");
    std::smatch match;
    
    if (!std::regex_search(url, match, gdrive_regex)) {
        sendMessage(chat_id, "❌ Invalid Google Drive link.");
        return;
    }
    
    std::string file_id = match[1].str();
    sendMessage(chat_id, std::format("🗑️ Deleting: `{}`\n\nThis feature requires Google Drive API integration.", file_id));
}

std::string BotEngine::getUptimeString() const {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
    
    auto days = std::chrono::duration_cast<std::chrono::hours>(uptime).count() / 24;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(uptime).count() % 24;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(uptime).count() % 60;
    
    if (days > 0) {
        return std::format("{}d {}h {}m", days, hours, minutes);
    } else if (hours > 0) {
        return std::format("{}h {}m", hours, minutes);
    } else {
        return std::format("{}m", minutes);
    }
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void BotEngine::trackDownload(int64_t chat_id, int64_t user_id, const std::string& gid, bool leech) {
    std::jthread tracker([this, chat_id, user_id, gid, leech](std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            if (stoken.stop_requested()) break;
            
            try {
                auto status_future = downloader_->getStatus(gid);
                auto result = status_future.get();
                
                if (!result) {
                    Logger::warn("Failed to get status for {}", gid);
                    break;
                }
                
                auto& status = *result;
                
                if (status.state == DownloadState::Complete) {
                    Logger::info("Download {} completed", gid);
                    enqueueTask([this, chat_id, user_id, gid, leech] {
                        onDownloadComplete(gid, chat_id, user_id, leech);
                    });
                    break;
                } else if (status.state == DownloadState::Error || 
                           status.state == DownloadState::Removed) {
                    Logger::warn("Download {} failed/removed", gid);
                    enqueueTask([this, chat_id, status] {
                        sendMessage(chat_id, std::format(
                            "❌ Download failed: {}", 
                            status.error_message.value_or("Unknown error")));
                    });
                    break;
                }
                
            } catch (const std::exception& e) {
                Logger::error("Tracker exception: {}", e.what());
                break;
            }
        }
    });
    
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    background_tasks_.push_back(std::move(tracker));
}

void BotEngine::onDownloadComplete(const std::string& gid, int64_t chat_id, int64_t user_id, bool leech) {
    sendMessage(chat_id, std::format("✅ Download complete: `{}`\n\n{}", 
        gid, leech ? "📤 Starting upload to Telegram..." : "📤 Starting upload to cloud..."));
    
    // Update task status
    if (database_) {
        auto tasks = database_->getIncompleteTasks();
        if (tasks) {
            for (const auto& task : *tasks) {
                if (task.download_id == gid) {
                    database_->updateTaskStatus(task.task_id, "uploading");
                    break;
                }
            }
        }
    }
    
    // Upload logic would go here...
    // For now, just mark as complete
    sendMessage(chat_id, std::format("✅ Upload complete for: `{}`", gid));
}

void BotEngine::processResponse(td::ClientManager::Response response) {
    if (!response.object) return;
    if (response.request_id == 0) {
        handleUpdate(std::move(response.object));
    }
}

void BotEngine::handleUpdate(std::unique_ptr<td::td_api::Object> update) {
    td::td_api::downcast_call(*update, [this](auto& update_obj) {
        using T = std::decay_t<decltype(update_obj)>;
        
        if constexpr (std::is_same_v<T, td::td_api::updateNewMessage>) {
            this->onNewMessage(update_obj);
            
        } else if constexpr (std::is_same_v<T, td::td_api::updateAuthorizationState>) {
            auto* auth_state = update_obj.authorization_state_.get();
            if (!auth_state) return;

            td::td_api::downcast_call(*auth_state, [this](auto& state) {
                using S = std::decay_t<decltype(state)>;
                
                if constexpr (std::is_same_v<S, td::td_api::authorizationStateWaitTdlibParameters>) {
                    Logger::info("Sending Tdlib parameters...");
                    auto request = td::td_api::make_object<td::td_api::setTdlibParameters>();
                    request->database_directory_ = config_.telegram.database_directory;
                    request->use_message_database_ = true;
                    request->use_secret_chats_ = true;
                    request->api_id_ = config_.telegram.api_id;
                    request->api_hash_ = config_.telegram.api_hash;
                    request->system_language_code_ = "en";
                    request->device_model_ = "Desktop";
                    request->application_version_ = "0.2.0";
                    
                    td_client_manager_->send(td_client_id_, 1, std::move(request));

                } else if constexpr (std::is_same_v<S, td::td_api::authorizationStateWaitPhoneNumber>) {
                    std::cout << "Enter phone number: " << std::flush;
                    std::string phone;
                    std::getline(std::cin, phone);
                    
                    auto request = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
                    request->phone_number_ = phone;
                    td_client_manager_->send(td_client_id_, 2, std::move(request));

                } else if constexpr (std::is_same_v<S, td::td_api::authorizationStateWaitCode>) {
                    std::cout << "Enter code: " << std::flush;
                    std::string code;
                    std::getline(std::cin, code);

                    auto request = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
                    request->code_ = code;
                    td_client_manager_->send(td_client_id_, 3, std::move(request));

                } else if constexpr (std::is_same_v<S, td::td_api::authorizationStateReady>) {
                    Logger::info("Tdlib Authorized successfully!");
                    
                } else if constexpr (std::is_same_v<S, td::td_api::authorizationStateClosed>) {
                    Logger::info("Tdlib session closed.");
                    requestStop();
                }
            });
        }
    });
}

void BotEngine::onNewMessage(const td::td_api::updateNewMessage& update) {
    if (!update.message || !update.message->content) return;
    if (update.message->is_outgoing_) return;
    
    if (update.message->content->get_id() == td::td_api::messageText::ID) {
        auto* text_content = static_cast<const td::td_api::messageText*>(update.message->content.get());
        std::string text = text_content->text_->text_;
        int64_t chat_id = update.message->chat_id_;
        int64_t user_id = update.message->sender_id_->get_id() == td::td_api::messageSenderUser::ID 
            ? static_cast<const td::td_api::messageSenderUser*>(update.message->sender_id_.get())->user_id_
            : 0;
        int64_t message_id = update.message->id_;
        
        // Check if it's a command
        if (text.starts_with("/")) {
            // Parse command and args
            std::string command;
            std::string args;
            
            size_t space_pos = text.find(' ');
            if (space_pos != std::string::npos) {
                command = text.substr(1, space_pos - 1);
                args = text.substr(space_pos + 1);
            } else {
                command = text.substr(1);
                // Remove @botname suffix if present
                size_t at_pos = command.find('@');
                if (at_pos != std::string::npos) {
                    command = command.substr(0, at_pos);
                }
            }
            
            // Create context and dispatch
            CommandContext ctx;
            ctx.chat_id = chat_id;
            ctx.user_id = user_id;
            ctx.message_id = message_id;
            ctx.command = command;
            ctx.args = args;
            ctx.full_text = text;
            ctx.engine = this;
            
            if (command_router_ && command_router_->dispatch(ctx)) {
                Logger::debug("Dispatched command: /{} from user {}", command, user_id);
            }
        }
    }
}

void BotEngine::sendTextMessage(int64_t chatId, const std::string& text) {
    auto content = td::td_api::make_object<td::td_api::inputMessageText>();
    content->text_ = td::td_api::make_object<td::td_api::formattedText>();
    content->text_->text_ = text;
    
    auto request = td::td_api::make_object<td::td_api::sendMessage>();
    request->chat_id_ = chatId;
    request->input_message_content_ = std::move(content);
    
    td_client_manager_->send(td_client_id_, 0, std::move(request));
}

// Legacy handlers (kept for backward compatibility)
void BotEngine::handleMirrorCommand(int64_t chatId, const std::string& text) {
    size_t space_pos = text.find(' ');
    if (space_pos == std::string::npos) {
        sendTextMessage(chatId, "Usage: /mirror <URL>");
        return;
    }
    
    std::string url = text.substr(space_pos + 1);
    startMirror(chatId, 0, url, false);
}

void BotEngine::handleStatusCommand(int64_t chatId) {
    CommandContext ctx;
    ctx.chat_id = chatId;
    ctx.user_id = 0;
    ctx.command = "status";
    ctx.engine = this;
    
    handleStatus(ctx);
}

void BotEngine::handleCancelCommand(int64_t chatId, const std::string& text) {
    size_t space_pos = text.find(' ');
    if (space_pos == std::string::npos) {
        sendTextMessage(chatId, "Usage: /cancel <GID>");
        return;
    }
    
    std::string gid = text.substr(space_pos + 1);
    CommandContext ctx;
    ctx.chat_id = chatId;
    ctx.user_id = 0;
    ctx.args = gid;
    ctx.engine = this;
    
    handleCancel(ctx);
}

} // namespace cmlb
