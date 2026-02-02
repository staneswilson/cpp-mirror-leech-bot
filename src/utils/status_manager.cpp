#include "utils/status_manager.hpp"
#include "core/logger.hpp"

#include <format>
#include <random>
#include <algorithm>
#include <sstream>

namespace cmlb {

StatusManager::StatusManager(const Config& config) : config_(config) {
    Logger::info("StatusManager initialized (interval={}s, per_page={})",
                 config_.update_interval_sec, config_.tasks_per_page);
}

StatusManager::~StatusManager() = default;

std::string StatusManager::generateTaskId() const {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    return std::format("{:016x}", dis(gen));
}

std::string StatusManager::addTask(const TaskInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string id = info.id.empty() ? generateTaskId() : info.id;
    
    TaskInfo task = info;
    task.id = id;
    task.started_at = std::chrono::system_clock::now();
    task.updated_at = task.started_at;
    
    tasks_[id] = task;
    last_update_[id] = std::chrono::steady_clock::now();
    
    Logger::debug("Added task {}: {}", id, info.name);
    return id;
}

void StatusManager::updateFromDownload(const std::string& task_id, const DownloadStatus& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    
    it->second.total_bytes = status.total_bytes;
    it->second.processed_bytes = status.downloaded_bytes;
    it->second.speed_bps = status.download_speed;
    it->second.eta = status.eta;
    it->second.updated_at = std::chrono::system_clock::now();
    
    switch (status.state) {
        case DownloadState::Downloading:
            it->second.state = TaskInfo::State::Active;
            break;
        case DownloadState::Paused:
            it->second.state = TaskInfo::State::Paused;
            break;
        case DownloadState::Complete:
            it->second.state = TaskInfo::State::Complete;
            break;
        case DownloadState::Error:
            it->second.state = TaskInfo::State::Error;
            it->second.error_message = status.error_message;
            break;
        default:
            break;
    }
}

void StatusManager::updateFromUpload(const std::string& task_id, const UploadProgress& progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    
    it->second.total_bytes = progress.total_bytes;
    it->second.processed_bytes = progress.uploaded_bytes;
    it->second.speed_bps = progress.speed_bps;
    it->second.eta = progress.eta;
    it->second.updated_at = std::chrono::system_clock::now();
    it->second.state = TaskInfo::State::Active;
}

void StatusManager::completeTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second.state = TaskInfo::State::Complete;
        it->second.updated_at = std::chrono::system_clock::now();
        Logger::info("Task {} completed", task_id);
    }
}

void StatusManager::failTask(const std::string& task_id, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second.state = TaskInfo::State::Error;
        it->second.error_message = error;
        it->second.updated_at = std::chrono::system_clock::now();
        Logger::error("Task {} failed: {}", task_id, error);
    }
}

void StatusManager::removeTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(task_id);
    last_update_.erase(task_id);
}

std::optional<TaskInfo> StatusManager::getTask(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<TaskInfo> StatusManager::getActiveTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<TaskInfo> result;
    for (const auto& [id, task] : tasks_) {
        if (task.state == TaskInfo::State::Active || 
            task.state == TaskInfo::State::Queued ||
            task.state == TaskInfo::State::Paused) {
            result.push_back(task);
        }
    }
    
    // Sort by start time
    std::sort(result.begin(), result.end(), [](const TaskInfo& a, const TaskInfo& b) {
        return a.started_at < b.started_at;
    });
    
    return result;
}

std::vector<TaskInfo> StatusManager::getUserTasks(int64_t user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<TaskInfo> result;
    for (const auto& [id, task] : tasks_) {
        if (task.user_id == user_id) {
            result.push_back(task);
        }
    }
    return result;
}

std::vector<TaskInfo> StatusManager::getPage(int page) const {
    auto active = getActiveTasks();
    
    int start = page * config_.tasks_per_page;
    int end = std::min(start + config_.tasks_per_page, static_cast<int>(active.size()));
    
    if (start >= static_cast<int>(active.size())) {
        return {};
    }
    
    return std::vector<TaskInfo>(active.begin() + start, active.begin() + end);
}

int StatusManager::getTotalPages() const {
    auto active = getActiveTasks();
    return (static_cast<int>(active.size()) + config_.tasks_per_page - 1) / config_.tasks_per_page;
}

std::string StatusManager::formatStatusMessage(int page) const {
    auto tasks = getPage(page);
    int total_pages = getTotalPages();
    
    if (tasks.empty()) {
        return "📭 No active tasks.";
    }
    
    std::ostringstream oss;
    oss << "📊 **Status** (Page " << (page + 1) << "/" << total_pages << ")\n\n";
    
    for (const auto& task : tasks) {
        // Type emoji
        switch (task.type) {
            case TaskInfo::Type::Download: oss << "⬇️ "; break;
            case TaskInfo::Type::Upload: oss << "⬆️ "; break;
            case TaskInfo::Type::Clone: oss << "📋 "; break;
            case TaskInfo::Type::Extract: oss << "📦 "; break;
            case TaskInfo::Type::Archive: oss << "🗜️ "; break;
        }
        
        // Name (truncated)
        std::string name = task.name.length() > 25 ? task.name.substr(0, 22) + "..." : task.name;
        oss << "**" << name << "**\n";
        
        // Progress bar
        oss << renderProgressBar(task.progress(), config_.progress_bar_width) << "\n";
        
        // Speed and ETA
        oss << formatSpeed(task.speed_bps) << " | ETA: " << formatDuration(task.eta) << "\n";
        
        // Size
        oss << formatBytes(task.processed_bytes) << " / " << formatBytes(task.total_bytes) << "\n\n";
    }
    
    // Footer with controls hint
    if (total_pages > 1) {
        oss << "Use ◀️ ▶️ to navigate pages";
    }
    
    return oss.str();
}

bool StatusManager::needsUpdate(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = last_update_.find(task_id);
    if (it == last_update_.end()) return true;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);
    
    return elapsed.count() >= config_.update_interval_sec;
}

void StatusManager::markUpdated(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_update_[task_id] = std::chrono::steady_clock::now();
}

// ============================================================================
// Static Rendering Utilities
// ============================================================================

std::string StatusManager::renderProgressBar(double percent, int width) {
    int filled = static_cast<int>(percent / 100.0 * width);
    int empty = width - filled;
    
    std::string bar;
    for (int i = 0; i < filled; i++) bar += "█";
    for (int i = 0; i < empty; i++) bar += "░";
    
    return std::format("[{}] {:.1f}%", bar, percent);
}

std::string StatusManager::formatBytes(int64_t bytes) {
    constexpr std::array<const char*, 5> units = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }
    
    return std::format("{:.2f} {}", size, units[unit_idx]);
}

std::string StatusManager::formatDuration(std::chrono::seconds duration) {
    if (duration.count() <= 0) return "N/A";
    
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    auto mins = std::chrono::duration_cast<std::chrono::minutes>(duration - hours);
    auto secs = duration - hours - mins;
    
    if (hours.count() > 0) {
        return std::format("{}h {}m", hours.count(), mins.count());
    } else if (mins.count() > 0) {
        return std::format("{}m {}s", mins.count(), secs.count());
    } else {
        return std::format("{}s", secs.count());
    }
}

std::string StatusManager::formatSpeed(int64_t bytes_per_sec) {
    return formatBytes(bytes_per_sec) + "/s";
}

} // namespace cmlb
