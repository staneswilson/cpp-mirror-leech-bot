#include "downloaders/aria2_client.hpp"
#include "core/logger.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <regex>
#include <chrono>
#include <filesystem>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

namespace cmlb {

// Helper to convert aria2 status string to enum
static DownloadState parseAria2Status(const std::string& status) {
    if (status == "active") return DownloadState::Downloading;
    if (status == "waiting") return DownloadState::Waiting;
    if (status == "paused") return DownloadState::Paused;
    if (status == "complete") return DownloadState::Complete;
    if (status == "error") return DownloadState::Error;
    if (status == "removed") return DownloadState::Removed;
    return DownloadState::Error;
}

// Helper to parse DownloadStatus from aria2 JSON response
static DownloadStatus parseDownloadStatus(const json& j) {
    DownloadStatus status;
    
    status.id = j.value("gid", "");
    
    // Get filename from files array or bittorrent info
    if (j.contains("bittorrent") && j["bittorrent"].contains("info")) {
        status.name = j["bittorrent"]["info"].value("name", "Unknown");
    } else if (j.contains("files") && !j["files"].empty()) {
        std::string path = j["files"][0].value("path", "");
        auto pos = path.find_last_of("/\\");
        status.name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    } else {
        status.name = "Unknown";
    }
    
    status.state = parseAria2Status(j.value("status", "error"));
    status.total_bytes = std::stoll(j.value("totalLength", "0"));
    status.downloaded_bytes = std::stoll(j.value("completedLength", "0"));
    status.uploaded_bytes = std::stoll(j.value("uploadLength", "0"));
    status.download_speed = std::stoll(j.value("downloadSpeed", "0"));
    status.upload_speed = std::stoll(j.value("uploadSpeed", "0"));
    
    if (status.download_speed > 0 && status.total_bytes > status.downloaded_bytes) {
        int64_t remaining = status.total_bytes - status.downloaded_bytes;
        status.eta = std::chrono::seconds(remaining / status.download_speed);
    }
    
    if (j.contains("errorMessage") && !j["errorMessage"].is_null()) {
        status.error_message = j["errorMessage"].get<std::string>();
    }
    
    if (j.contains("dir")) {
        status.save_path = j["dir"].get<std::string>();
    }
    
    if (j.contains("numSeeders")) {
        status.num_seeders = std::stoi(j.value("numSeeders", "0"));
    }
    
    return status;
}

// Helper to get all file paths from a download
static std::vector<std::string> getDownloadFiles(const json& j) {
    std::vector<std::string> files;
    if (j.contains("files")) {
        for (const auto& file : j["files"]) {
            if (file.contains("path")) {
                files.push_back(file["path"].get<std::string>());
            }
        }
    }
    return files;
}

class Aria2Downloader::Impl {
public:
    using ResponseCallback = std::move_only_function<void(Result<json>)>;

private:
    Aria2Downloader::Config config_;
    
    net::io_context ioc_;
    net::executor_work_guard<net::io_context::executor_type> work_guard_;
    std::jthread worker_thread_;
    
    websocket::stream<tcp::socket> ws_;
    std::string host_;
    std::string port_;
    std::string path_;

    std::unordered_map<int64_t, ResponseCallback> callbacks_;
    std::mutex map_mutex_;
    std::atomic<int64_t> next_id_{1};
    std::atomic<bool> connected_{false};
    std::atomic<bool> shutdown_{false};

    beast::flat_buffer buffer_;

    int reconnect_attempts_{0};
    std::chrono::milliseconds current_backoff_{500};
    static constexpr std::chrono::milliseconds kMaxBackoff{30000};

public:
    explicit Impl(const Aria2Downloader::Config& config)
        : config_(config), work_guard_(net::make_work_guard(ioc_)), ws_(ioc_)
    {
        parseUrl(config_.rpc_url);
        
        worker_thread_ = std::jthread([this](std::stop_token stoken) {
            while (!stoken.stop_requested()) {
                try {
                    ioc_.run();
                    break;
                } catch (const std::exception& e) {
                    Logger::error("Aria2 IO exception: {}", e.what());
                    if (!shutdown_.load()) ioc_.restart();
                }
            }
        });

        initiateConnection();
    }

    ~Impl() {
        shutdown_.store(true);
        if (connected_.load()) {
            beast::error_code ec;
            ws_.close(websocket::close_code::normal, ec);
        }
        work_guard_.reset();
        ioc_.stop();
    }

    bool isConnected() const noexcept { return connected_.load(); }

    // Generic request sender
    template<typename Converter>
    std::future<Result<typename std::invoke_result_t<Converter, json>>> 
    sendRequest(const std::string& method, const json& params, Converter converter) {
        using ReturnType = typename std::invoke_result_t<Converter, json>;
        auto promise = std::make_shared<std::promise<Result<ReturnType>>>();
        auto future = promise->get_future();

        net::post(ioc_, [this, method, params, converter = std::move(converter), 
                         promise = std::move(promise)]() mutable {
            if (!connected_.load()) {
                promise->set_value(std::unexpected(
                    AppError(ErrorCode::Aria2ConnectionFailed, "Not connected")));
                return;
            }

            int64_t id = next_id_.fetch_add(1);
            
            json request;
            request["jsonrpc"] = "2.0";
            request["id"] = id;
            request["method"] = method;
            request["params"] = json::array();
            
            if (!config_.secret.empty()) {
                request["params"].push_back("token:" + config_.secret);
            }
            for (const auto& p : params) {
                request["params"].push_back(p);
            }

            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                callbacks_.emplace(id, [promise, converter](Result<json> res) {
                    if (!res) {
                        promise->set_value(std::unexpected(res.error()));
                    } else {
                        try {
                            promise->set_value(converter(*res));
                        } catch (const std::exception& e) {
                            promise->set_value(std::unexpected(
                                AppError(ErrorCode::JsonParseError, e.what())));
                        }
                    }
                });
            }

            std::string payload = request.dump();
            ws_.async_write(net::buffer(payload), 
                [this, id](beast::error_code ec, std::size_t) {
                    if (ec) {
                        Logger::error("Write error: {}", ec.message());
                        std::lock_guard<std::mutex> lock(map_mutex_);
                        auto it = callbacks_.find(id);
                        if (it != callbacks_.end()) {
                            it->second(std::unexpected(
                                AppError(ErrorCode::NetworkError, ec.message())));
                            callbacks_.erase(it);
                        }
                    }
                });
        });

        return future;
    }

    // Void-returning request helper
    std::future<Result<void>> sendVoidRequest(const std::string& method, const json& params) {
        return sendRequest(method, params, [](const json&) -> void {});
    }

    // Request that returns the full JSON result (for file deletion)
    std::future<Result<json>> sendJsonRequest(const std::string& method, const json& params) {
        return sendRequest(method, params, [](const json& res) -> json { return res; });
    }

private:
    void parseUrl(const std::string& url) {
        std::regex url_regex(R"(^wss?://([^:/]+)(?::(\d+))?(/.*)?$)");
        std::smatch match;
        
        if (std::regex_match(url, match, url_regex)) {
            host_ = match[1].str();
            port_ = match[2].matched ? match[2].str() : "6800";
            path_ = match[3].matched ? match[3].str() : "/jsonrpc";
        } else {
            host_ = "localhost";
            port_ = "6800";
            path_ = "/jsonrpc";
        }
    }

    void initiateConnection() {
        if (shutdown_.load()) return;
        
        Logger::info("Connecting to Aria2 at {}:{}...", host_, port_);
        
        try {
            tcp::resolver resolver(ioc_);
            auto const results = resolver.resolve(host_, port_);
            
            net::async_connect(ws_.next_layer(), results, 
                [this](beast::error_code ec, tcp::endpoint) {
                    if (ec) {
                        Logger::error("Connect failed: {}", ec.message());
                        scheduleReconnect();
                        return;
                    }
                    performHandshake();
                });
        } catch (const std::exception& e) {
            Logger::error("DNS resolution failed: {}", e.what());
            scheduleReconnect();
        }
    }

    void performHandshake() {
        ws_.async_handshake(host_, path_, [this](beast::error_code ec) {
            if (ec) {
                Logger::error("Handshake failed: {}", ec.message());
                scheduleReconnect();
                return;
            }
            
            connected_.store(true);
            reconnect_attempts_ = 0;
            current_backoff_ = std::chrono::milliseconds{500};
            
            Logger::info("Connected to Aria2");
            startReadLoop();
        });
    }

    void scheduleReconnect() {
        if (shutdown_.load()) return;
        if (reconnect_attempts_ >= config_.max_reconnect_attempts) {
            Logger::error("Max reconnection attempts reached");
            return;
        }
        
        reconnect_attempts_++;
        Logger::info("Reconnecting in {}ms...", current_backoff_.count());
        
        auto timer = std::make_shared<net::steady_timer>(ioc_, current_backoff_);
        timer->async_wait([this, timer](beast::error_code ec) {
            if (!ec && !shutdown_.load()) {
                ws_ = websocket::stream<tcp::socket>(ioc_);
                initiateConnection();
            }
        });
        
        current_backoff_ = std::min(current_backoff_ * 2, kMaxBackoff);
    }

    void startReadLoop() {
        if (shutdown_.load() || !connected_.load()) return;
        
        ws_.async_read(buffer_, [this](beast::error_code ec, std::size_t) {
            if (ec) {
                if (ec != websocket::error::closed) {
                    Logger::error("Read error: {}", ec.message());
                }
                connected_.store(false);
                scheduleReconnect();
                return;
            }

            processMessage();
            buffer_.consume(buffer_.size());
            startReadLoop();
        });
    }

    void processMessage() {
        try {
            std::string data = beast::buffers_to_string(buffer_.data());
            json j = json::parse(data);
            
            if (!j.contains("id") || !j["id"].is_number_integer()) return;
            
            int64_t id = j["id"].get<int64_t>();
            ResponseCallback cb;
            
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                auto it = callbacks_.find(id);
                if (it != callbacks_.end()) {
                    cb = std::move(it->second);
                    callbacks_.erase(it);
                }
            }
            
            if (!cb) return;
            
            if (j.contains("error") && !j["error"].is_null()) {
                std::string msg = j["error"].value("message", "Unknown error");
                cb(std::unexpected(AppError(ErrorCode::Aria2ConnectionFailed, msg)));
            } else if (j.contains("result")) {
                cb(j["result"]);
            }
            
        } catch (const json::exception& e) {
            Logger::error("JSON parse error: {}", e.what());
        }
    }
};

// ============================================================================
// Aria2Downloader Public Interface
// ============================================================================

Aria2Downloader::Aria2Downloader(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

Aria2Downloader::~Aria2Downloader() = default;

bool Aria2Downloader::isConnected() const noexcept {
    return impl_->isConnected();
}

std::future<Result<std::string>> Aria2Downloader::addUri(
    std::string_view uri,
    const std::vector<std::pair<std::string, std::string>>& options) 
{
    json opts = json::object();
    for (const auto& [k, v] : options) opts[k] = v;
    
    return impl_->sendRequest("aria2.addUri", 
        json::array({ json::array({std::string(uri)}), opts }),
        [](const json& res) -> std::string {
            return res.get<std::string>();
        });
}

std::future<Result<std::string>> Aria2Downloader::addTorrent(
    std::string_view torrent_data,
    const std::vector<std::pair<std::string, std::string>>& options) 
{
    // Base64 encode the torrent data
    // For simplicity, assuming torrent_data is already base64 encoded
    json opts = json::object();
    for (const auto& [k, v] : options) opts[k] = v;
    
    return impl_->sendRequest("aria2.addTorrent", 
        json::array({ std::string(torrent_data), json::array(), opts }),
        [](const json& res) -> std::string {
            return res.get<std::string>();
        });
}

std::future<Result<void>> Aria2Downloader::pause(std::string_view id) {
    return impl_->sendVoidRequest("aria2.pause", json::array({std::string(id)}));
}

std::future<Result<void>> Aria2Downloader::resume(std::string_view id) {
    return impl_->sendVoidRequest("aria2.unpause", json::array({std::string(id)}));
}

std::future<Result<void>> Aria2Downloader::remove(std::string_view id, bool delete_files) {
    std::string gid(id);
    
    // Create a promise for the final result
    auto promise = std::make_shared<std::promise<Result<void>>>();
    auto future = promise->get_future();
    
    // First get the file paths if we need to delete files
    if (delete_files) {
        auto status_future = impl_->sendJsonRequest("aria2.tellStatus", json::array({gid}));
        
        std::thread([this, gid, promise, status_future = std::move(status_future)]() mutable {
            try {
                auto status_result = status_future.get();
                std::vector<std::string> file_paths;
                
                if (status_result) {
                    file_paths = getDownloadFiles(*status_result);
                }
                
                // Now remove from aria2
                auto remove_future = impl_->sendVoidRequest("aria2.remove", json::array({gid}));
                auto remove_result = remove_future.get();
                
                if (!remove_result) {
                    // Try forceRemove if normal remove fails
                    auto force_future = impl_->sendVoidRequest("aria2.forceRemove", json::array({gid}));
                    remove_result = force_future.get();
                }
                
                // Delete the files
                if (remove_result) {
                    for (const auto& path : file_paths) {
                        std::error_code ec;
                        if (std::filesystem::exists(path, ec)) {
                            std::filesystem::remove_all(path, ec);
                            if (!ec) {
                                Logger::debug("Deleted: {}", path);
                            }
                        }
                    }
                    promise->set_value({});
                } else {
                    promise->set_value(std::unexpected(remove_result.error()));
                }
                
            } catch (const std::exception& e) {
                promise->set_value(std::unexpected(AppError(ErrorCode::Unknown, e.what())));
            }
        }).detach();
        
    } else {
        // Just remove from aria2, don't delete files
        return impl_->sendVoidRequest("aria2.remove", json::array({gid}));
    }
    
    return future;
}

std::future<Result<void>> Aria2Downloader::forcePause(std::string_view gid) {
    return impl_->sendVoidRequest("aria2.forcePause", json::array({std::string(gid)}));
}

std::future<Result<void>> Aria2Downloader::forceRemove(std::string_view gid) {
    return impl_->sendVoidRequest("aria2.forceRemove", json::array({std::string(gid)}));
}

std::future<Result<DownloadStatus>> Aria2Downloader::getStatus(std::string_view id) {
    return impl_->sendRequest("aria2.tellStatus", 
        json::array({std::string(id)}),
        [](const json& res) -> DownloadStatus {
            return parseDownloadStatus(res);
        });
}

std::future<Result<std::vector<DownloadStatus>>> Aria2Downloader::getActiveDownloads() {
    return impl_->sendRequest("aria2.tellActive", 
        json::array(),
        [](const json& res) -> std::vector<DownloadStatus> {
            std::vector<DownloadStatus> result;
            for (const auto& item : res) {
                result.push_back(parseDownloadStatus(item));
            }
            return result;
        });
}

std::future<Result<GlobalStats>> Aria2Downloader::getGlobalStats() {
    return impl_->sendRequest("aria2.getGlobalStat", 
        json::array(),
        [](const json& res) -> GlobalStats {
            GlobalStats stats;
            stats.download_speed = std::stoll(res.value("downloadSpeed", "0"));
            stats.upload_speed = std::stoll(res.value("uploadSpeed", "0"));
            stats.active_count = std::stoi(res.value("numActive", "0"));
            stats.waiting_count = std::stoi(res.value("numWaiting", "0"));
            stats.stopped_count = std::stoi(res.value("numStopped", "0"));
            return stats;
        });
}

std::future<Result<std::string>> Aria2Downloader::getVersion() {
    return impl_->sendRequest("aria2.getVersion", 
        json::array(),
        [](const json& res) -> std::string {
            return res.value("version", "unknown");
        });
}

std::future<Result<void>> Aria2Downloader::changeGlobalOptions(
    const std::vector<std::pair<std::string, std::string>>& options) 
{
    json opts = json::object();
    for (const auto& [k, v] : options) opts[k] = v;
    return impl_->sendVoidRequest("aria2.changeGlobalOption", json::array({opts}));
}

std::future<Result<void>> Aria2Downloader::shutdown() {
    return impl_->sendVoidRequest("aria2.shutdown", json::array());
}

std::unique_ptr<Aria2Downloader> Aria2Downloader::create(const Config& config) {
    return std::make_unique<Aria2Downloader>(config);
}

} // namespace cmlb
