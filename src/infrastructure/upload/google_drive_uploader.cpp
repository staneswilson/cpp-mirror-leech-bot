#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <cmlb/core/executor.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/infrastructure/http/beast_http_client.hpp>
#include <cmlb/infrastructure/upload/google_drive_uploader.hpp>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace cmlb::infrastructure::upload {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::int64_t kMultipartThreshold = 5LL * 1024 * 1024; // 5 MiB
constexpr std::int64_t kResumableAlignment = 256 * 1024;        // 256 KiB

// ---------------------------------------------------------------------------
// Base64url helpers (no padding).
// ---------------------------------------------------------------------------

[[nodiscard]] std::string base64url(std::span<const unsigned char> data) {
    // Standard base64 via OpenSSL then translate alphabet + strip padding.
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* sink = BIO_new(BIO_s_mem());
    BIO_push(b64, sink);
    BIO_write(b64, data.data(), static_cast<int>(data.size()));
    BIO_flush(b64);
    BUF_MEM* ptr = nullptr;
    BIO_get_mem_ptr(b64, &ptr);
    std::string out{ptr->data, ptr->length};
    BIO_free_all(b64);

    std::ranges::replace(out, '+', '-');
    std::ranges::replace(out, '/', '_');
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    return out;
}

[[nodiscard]] std::string base64url(std::string_view sv) {
    return base64url({reinterpret_cast<const unsigned char*>(sv.data()), sv.size()});
}

[[nodiscard]] std::string openssl_last_error() {
    unsigned long err = ERR_get_error();
    if (err == 0)
        return "unknown OpenSSL error";
    std::array<char, 256> buf{};
    ERR_error_string_n(err, buf.data(), buf.size());
    return std::string{buf.data()};
}

// ---------------------------------------------------------------------------
// RS256 signing.
// ---------------------------------------------------------------------------

[[nodiscard]] cmlb::core::Result<std::string> rs256_sign(std::string_view signing_input,
                                                         std::string_view pem_private_key) {
    BIO* bio = BIO_new_mem_buf(pem_private_key.data(), static_cast<int>(pem_private_key.size()));
    if (bio == nullptr) {
        return cmlb::core::error(cmlb::core::ErrorCode::Internal, "gdrive: BIO_new_mem_buf failed");
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidConfiguration,
                                 "gdrive: PEM_read_bio_PrivateKey failed: " + openssl_last_error());
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        EVP_PKEY_free(pkey);
        return cmlb::core::error(cmlb::core::ErrorCode::Internal, "gdrive: EVP_MD_CTX_new failed");
    }

    cmlb::core::Result<std::string> result = cmlb::core::error(
        cmlb::core::ErrorCode::Internal, "gdrive: signing did not produce output");

    do {
        if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            result =
                cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                  "gdrive: EVP_DigestSignInit failed: " + openssl_last_error());
            break;
        }
        if (EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) <= 0) {
            result =
                cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                  "gdrive: EVP_DigestSignUpdate failed: " + openssl_last_error());
            break;
        }
        std::size_t sig_len = 0;
        if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) <= 0) {
            result = cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                       "gdrive: EVP_DigestSignFinal(size) failed: "
                                           + openssl_last_error());
            break;
        }
        std::vector<unsigned char> sig(sig_len);
        if (EVP_DigestSignFinal(ctx, sig.data(), &sig_len) <= 0) {
            result =
                cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                  "gdrive: EVP_DigestSignFinal failed: " + openssl_last_error());
            break;
        }
        sig.resize(sig_len);
        result = base64url({sig.data(), sig.size()});
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return result;
}

// ---------------------------------------------------------------------------
// Service-account credentials.
// ---------------------------------------------------------------------------

[[nodiscard]] cmlb::core::Result<ServiceAccountKey> load_service_account(
    const fs::path& credentials_path) {
    std::ifstream in{credentials_path};
    if (!in) {
        return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                 "gdrive: cannot open credentials file "
                                     + credentials_path.string());
    }
    json doc;
    try {
        in >> doc;
    } catch (const std::exception& e) {
        return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                 std::string{"gdrive: credentials JSON parse failed: "} + e.what());
    }

    ServiceAccountKey key;
    try {
        key.client_email = doc.at("client_email").get<std::string>();
        key.private_key_pem = doc.at("private_key").get<std::string>();
        if (doc.contains("token_uri")) {
            key.token_uri = doc.at("token_uri").get<std::string>();
        }
    } catch (const std::exception& e) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidConfiguration,
                                 std::string{"gdrive: missing field in credentials: "} + e.what());
    }
    if (key.client_email.empty() || key.private_key_pem.empty()) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidConfiguration,
                                 "gdrive: client_email / private_key must be non-empty");
    }
    return key;
}

using http::BeastHttpClient;
using http::HttpHeader;
using http::HttpMethod;
using http::HttpRequest;
using http::HttpResponse;

[[nodiscard]] HttpRequest make_request(HttpMethod method, std::string url) {
    HttpRequest req;
    req.method = method;
    req.url = std::move(url);
    return req;
}

void add_header(HttpRequest& req, std::string name, std::string value) {
    req.headers.push_back(HttpHeader{std::move(name), std::move(value)});
}

[[nodiscard]] std::optional<std::string> header_lookup(const HttpResponse& res,
                                                       std::string_view name) {
    for (const auto& h : res.headers) {
        if (h.name.size() == name.size() && std::ranges::equal(h.name, name, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a))
                       == std::tolower(static_cast<unsigned char>(b));
            })) {
            return h.value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string url_escape(std::string_view in) {
    static constexpr std::string_view safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    std::string out;
    out.reserve(in.size());
    for (char raw : in) {
        const auto c = static_cast<unsigned char>(raw);
        if (safe.find(raw) != std::string_view::npos) {
            out.push_back(raw);
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

} // namespace

// --------------------------------------------------------------------------
// GoogleDriveUploader
// --------------------------------------------------------------------------

GoogleDriveUploader::GoogleDriveUploader(cmlb::core::Executor& exec,
                                         cmlb::core::GoogleDriveConfig config,
                                         BeastHttpClient& http_client)
    : exec_{exec}, config_{std::move(config)}, http_{http_client} {
    auto loaded = load_service_account(config_.credentials_path);
    if (!loaded) {
        cmlb::core::Logger::error("gdrive: credentials unavailable: {}", loaded.error().message);
        ready_ = false;
        return;
    }
    key_ = std::move(*loaded);
    ready_ = true;
}

cmlb::core::Result<std::string> GoogleDriveUploader::build_signed_jwt() const {
    const auto now = std::chrono::system_clock::now();
    const auto iat =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto exp = iat + 3600;

    const json header = {{"alg", "RS256"}, {"typ", "JWT"}};
    const json claims = {{"iss", key_.client_email},
                         {"scope", "https://www.googleapis.com/auth/drive"},
                         {"aud", key_.token_uri},
                         {"exp", exp},
                         {"iat", iat}};

    const std::string signing_input = base64url(header.dump()) + "." + base64url(claims.dump());

    auto sig = rs256_sign(signing_input, key_.private_key_pem);
    if (!sig)
        return std::unexpected(sig.error());
    return signing_input + "." + *sig;
}

boost::asio::awaitable<cmlb::core::Result<DriveAccessToken>> GoogleDriveUploader::mint_bearer() {
    auto jwt = build_signed_jwt();
    if (!jwt)
        co_return std::unexpected(jwt.error());

    auto req = make_request(HttpMethod::Post, key_.token_uri);
    add_header(req, "Content-Type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" + url_escape(*jwt);

    auto res = co_await http_.request(std::move(req));
    if (!res)
        co_return std::unexpected(res.error());
    if (res->status_code != 200) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::Unauthenticated,
                                    fmt::format("gdrive: token exchange "
                                                "returned {}: {}",
                                                res->status_code,
                                                res->body));
    }
    try {
        json doc = json::parse(res->body);
        std::string at = doc.at("access_token").get<std::string>();
        int expires = doc.value("expires_in", 3600);
        DriveAccessToken tok;
        tok.value = "Bearer " + at;
        tok.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(expires);
        co_return tok;
    } catch (const std::exception& e) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                    std::string{"gdrive: token response parse failed: "}
                                        + e.what());
    }
}

boost::asio::awaitable<cmlb::core::Result<std::string>> GoogleDriveUploader::acquire_bearer() {
    {
        std::lock_guard lk{token_mutex_};
        const auto now = std::chrono::steady_clock::now();
        if (!cached_token_.value.empty()
            && cached_token_.expires_at - std::chrono::seconds(60) > now) {
            co_return cached_token_.value;
        }
    }
    auto fresh = co_await mint_bearer();
    if (!fresh)
        co_return std::unexpected(fresh.error());
    {
        std::lock_guard lk{token_mutex_};
        cached_token_ = *fresh;
        co_return cached_token_.value;
    }
}

void GoogleDriveUploader::invalidate_bearer_cache() noexcept {
    std::lock_guard lk{token_mutex_};
    cached_token_ = DriveAccessToken{};
}

// ----- multipart upload (<= 5 MiB) ----------------------------------------

boost::asio::awaitable<cmlb::core::Result<UploadResult>> GoogleDriveUploader::upload_multipart(
    fs::path path, const std::string& parent_folder, UploadProgressHandler on_progress) {
    namespace asio = boost::asio;
    const auto started = std::chrono::steady_clock::now();

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::Io,
                                    "gdrive: cannot open " + path.string());
    }
    std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    const json meta = {{"name", path.filename().string()},
                       {"parents", json::array({parent_folder})}};

    constexpr std::string_view boundary = "cmlb-mp-7af3b1-XXyy";
    std::string mp;
    mp += fmt::format("--{}\r\n", boundary);
    mp += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
    mp += meta.dump();
    mp += fmt::format("\r\n--{}\r\nContent-Type: application/octet-stream\r\n\r\n", boundary);
    mp += body;
    mp += fmt::format("\r\n--{}--\r\n", boundary);

    // `supportsAllDrives=true` so uploads land correctly in shared drives.
    // `fields=` trims the API response to ~80 bytes instead of the full
    // metadata blob — at high upload rates the response parse + HTTP read
    // back-pressure dominates.
    auto req = make_request(HttpMethod::Post,
                            "https://www.googleapis.com/upload/drive/v3/files"
                            "?uploadType=multipart&supportsAllDrives=true"
                            "&fields=id,name,size,webViewLink,mimeType");
    add_header(req, "Authorization", *bearer);
    add_header(req, "Content-Type", fmt::format("multipart/related; boundary={}", boundary));
    req.body = std::move(mp);

    if (on_progress) {
        UploadProgress p;
        p.file_name = path.filename().string();
        p.total_bytes = static_cast<std::int64_t>(body.size());
        p.uploaded_bytes = 0;
        on_progress(p);
    }

    auto res = co_await http_.request(std::move(req));
    if (!res)
        co_return std::unexpected(res.error());
    if (res->status_code / 100 != 2) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::GoogleDriveApi,
            fmt::format("gdrive: multipart upload returned {}: {}", res->status_code, res->body));
    }

    UploadResult out;
    try {
        json doc = json::parse(res->body);
        out.file_id = doc.at("id").get<std::string>();
    } catch (const std::exception& e) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                    std::string{"gdrive: multipart response parse: "} + e.what());
    }
    out.link = "https://drive.google.com/file/d/" + out.file_id + "/view";
    out.size = static_cast<std::int64_t>(body.size());
    out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    if (on_progress) {
        UploadProgress p;
        p.file_name = path.filename().string();
        p.total_bytes = out.size;
        p.uploaded_bytes = out.size;
        on_progress(p);
    }
    co_return out;
}

// ----- resumable upload (> 5 MiB) -----------------------------------------

boost::asio::awaitable<cmlb::core::Result<UploadResult>> GoogleDriveUploader::upload_resumable(
    fs::path path, const std::string& parent_folder, UploadProgressHandler on_progress) {
    namespace asio = boost::asio;
    const auto started = std::chrono::steady_clock::now();

    std::error_code ec;
    const auto raw_size = fs::file_size(path, ec);
    if (ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                    "gdrive: file_size failed: " + ec.message());
    }
    const std::int64_t total = static_cast<std::int64_t>(raw_size);

    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    // ---- Step 1: initiate the session ----
    const json meta = {{"name", path.filename().string()},
                       {"parents", json::array({parent_folder})}};
    // Same payload-trim + shared-drive flags as the multipart path. The
    // initial POST gets us a session URI; subsequent PUTs ride that URI and
    // are unaffected by these query params.
    auto init = make_request(HttpMethod::Post,
                             "https://www.googleapis.com/upload/drive/v3/files"
                             "?uploadType=resumable&supportsAllDrives=true"
                             "&fields=id,name,size,webViewLink,mimeType");
    add_header(init, "Authorization", *bearer);
    add_header(init, "Content-Type", "application/json; charset=UTF-8");
    add_header(init, "X-Upload-Content-Type", "application/octet-stream");
    add_header(init, "X-Upload-Content-Length", std::to_string(total));
    init.body = meta.dump();

    auto init_res = co_await http_.request(std::move(init));
    if (!init_res)
        co_return std::unexpected(init_res.error());
    if (init_res->status_code / 100 != 2) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::GoogleDriveApi,
                                    fmt::format("gdrive: resumable init returned {}: {}",
                                                init_res->status_code,
                                                init_res->body));
    }
    auto loc = header_lookup(*init_res, "location");
    if (!loc) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::GoogleDriveApi,
                                    "gdrive: resumable session missing Location header");
    }
    const std::string& session_uri = *loc;

    // ---- Step 2: upload aligned chunks ----
    std::int64_t chunk_bytes = static_cast<std::int64_t>(config_.chunk_size);
    chunk_bytes = std::max(chunk_bytes, kResumableAlignment);
    chunk_bytes = (chunk_bytes / kResumableAlignment) * kResumableAlignment;

    // Readability probe — workers each open their own handle since seekg+read
    // on a shared ifstream isn't thread-safe.
    {
        std::ifstream probe{path, std::ios::binary};
        if (!probe) {
            co_await abort_resumable_session(session_uri);
            co_return cmlb::core::error(cmlb::core::ErrorCode::Io,
                                        "gdrive: cannot open " + path.string());
        }
    }

    // Pre-compute the chunk index plan once — every worker claims chunks by
    // index, not by reading offset/length out of band.
    struct ChunkPlan {
        std::int64_t offset;
        std::int64_t length;
    };

    std::vector<ChunkPlan> plan;
    plan.reserve(static_cast<std::size_t>((total + chunk_bytes - 1) / chunk_bytes));
    for (std::int64_t off = 0; off < total; off += chunk_bytes) {
        plan.push_back(ChunkPlan{off, std::min<std::int64_t>(chunk_bytes, total - off)});
    }
    const int num_chunks = static_cast<int>(plan.size());

    const int parallelism =
        std::clamp(config_.parallel_chunks_per_file, 1, std::max(1, num_chunks));

    std::string final_file_id;

    {
        struct Shared {
            std::atomic<int> next_index;
            std::atomic<int> live_workers;
            std::atomic<std::int64_t> bytes_uploaded;
            std::atomic<bool> abort;
            std::mutex err_mtx;
            std::optional<cmlb::core::AppError> first_error;
            std::mutex fid_mtx;
            std::string file_id;
            asio::steady_timer join_timer;

            Shared(asio::any_io_executor ex, int workers)
                : next_index{0},
                  live_workers{workers},
                  bytes_uploaded{0},
                  abort{false},
                  join_timer{ex} {
                join_timer.expires_at(std::chrono::steady_clock::time_point::max());
            }
        };

        auto coro_exec = co_await asio::this_coro::executor;
        auto shared = std::make_shared<Shared>(coro_exec, parallelism);

        auto record_error = [shared](cmlb::core::AppError err) {
            std::lock_guard lk{shared->err_mtx};
            if (!shared->first_error)
                shared->first_error = std::move(err);
            shared->abort.store(true, std::memory_order_release);
        };

        auto plan_ptr = std::make_shared<std::vector<ChunkPlan>>(plan);
        const std::string path_str = path.string();

        // Worker coroutine: pulls the next chunk index, opens its own file
        // handle, PUTs to the session URI. One worker per slot — the last to
        // finish cancels the join timer so the main coroutine wakes. The
        // lambda is handed straight to `co_spawn`, which moves it into its
        // own handler storage, so the captures survive the loop iteration
        // that spawned them.
        //
        // Bearer is re-acquired per PUT via `acquire_bearer()` rather than
        // captured once. The cache hit is a single mutex + steady_clock
        // compare, so this is cheap; the win is that a token expiry during a
        // long upload (3600 s default lifetime) triggers a transparent refresh
        // on the next chunk instead of 401-ing every in-flight worker.
        auto worker_factory = [this, shared, plan_ptr, session_uri, total, path_str, record_error]()
            -> asio::awaitable<void> {
            // Worker lifecycle invariant: `live_workers` MUST be decremented
            // exactly once and the join timer cancelled when this worker is
            // the last to exit — even if the worker body throws. A leaked
            // worker would leave the parent coroutine sleeping forever on
            // the join timer. The exit_guard captures `shared` by value so
            // its destructor runs whether the worker returns normally,
            // throws, or is cancelled.
            struct ExitGuard {
                std::shared_ptr<Shared> shared;
                ~ExitGuard() noexcept {
                    if (shared->live_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        shared->join_timer.cancel();
                    }
                }
            };
            ExitGuard exit_guard{shared};
            std::ifstream local{path_str, std::ios::binary};
            std::string body_buffer;

            auto bail = [&](cmlb::core::AppError err) {
                record_error(std::move(err));
            };

            if (!local) {
                bail(cmlb::core::AppError{cmlb::core::ErrorCode::Io,
                                          "gdrive: worker cannot open " + path_str});
            } else {
                while (!shared->abort.load(std::memory_order_acquire)) {
                    if ((co_await asio::this_coro::cancellation_state).cancelled()
                        != asio::cancellation_type::none) {
                        shared->abort.store(true, std::memory_order_release);
                        break;
                    }
                    const int idx = shared->next_index.fetch_add(1, std::memory_order_acq_rel);
                    if (std::cmp_greater_equal(idx, plan_ptr->size()))
                        break;
                    const auto& cp = (*plan_ptr)[static_cast<std::size_t>(idx)];

                    body_buffer.resize(static_cast<std::size_t>(cp.length));
                    local.seekg(cp.offset);
                    local.read(body_buffer.data(), cp.length);
                    const auto got = local.gcount();
                    if (got != cp.length) {
                        bail(cmlb::core::AppError{cmlb::core::ErrorCode::Io,
                                                  "gdrive: worker short read on " + path_str});
                        break;
                    }

                    // PUT this chunk with two layers of retry:
                    //   * 401 → force-refresh the bearer cache + retry once.
                    //     Sibling workers pick up the refreshed token on their
                    //     next iteration.
                    //   * 5xx / 429 / network error → exponential backoff up
                    //     to `config_.max_retries`, doubling from
                    //     `config_.initial_retry_delay` each attempt.
                    //   * 4xx (other than 401/429) → fail-fast.
                    int code = 0;
                    std::string chunk_body;
                    std::optional<cmlb::core::AppError> chunk_err;
                    bool chunk_done = false;
                    const int max_retries = std::max(0, config_.max_retries);

                    for (int attempt = 0; attempt <= max_retries && !chunk_done; ++attempt) {
                        if (attempt > 0) {
                            const auto shift = std::min(attempt - 1, 10);
                            const auto delay = config_.initial_retry_delay * (1ULL << shift);
                            asio::steady_timer backoff{co_await asio::this_coro::executor};
                            backoff.expires_after(delay);
                            boost::system::error_code wec;
                            co_await backoff.async_wait(
                                asio::redirect_error(asio::use_awaitable, wec));
                            if (shared->abort.load(std::memory_order_acquire)) {
                                chunk_done = true;
                                break;
                            }
                        }

                        std::optional<cmlb::core::Result<cmlb::infrastructure::http::HttpResponse>>
                            chunk_res;
                        for (int auth_attempt = 0; auth_attempt < 2; ++auth_attempt) {
                            auto bearer_now = co_await acquire_bearer();
                            if (!bearer_now) {
                                chunk_err = bearer_now.error();
                                chunk_done = true;
                                break;
                            }
                            auto chunk = make_request(HttpMethod::Put, session_uri);
                            add_header(chunk, "Authorization", *bearer_now);
                            add_header(chunk, "Content-Length", std::to_string(got));
                            add_header(
                                chunk,
                                "Content-Range",
                                fmt::format(
                                    "bytes {}-{}/{}", cp.offset, cp.offset + got - 1, total));
                            chunk.body = body_buffer;

                            chunk_res = co_await http_.request(std::move(chunk));
                            if (!chunk_res->has_value())
                                break;
                            code = (*chunk_res)->status_code;
                            chunk_body = std::move((*chunk_res)->body);
                            if (code != 401)
                                break;
                            invalidate_bearer_cache();
                        }
                        if (chunk_done)
                            break;

                        if (!chunk_res->has_value()) {
                            // Cancellation is terminal — propagate immediately
                            // rather than burning retries on a request the
                            // user explicitly aborted. Other transport errors
                            // remain transient and retry with backoff.
                            if (chunk_res->error().code == cmlb::core::ErrorCode::Cancelled) {
                                chunk_err = chunk_res->error();
                                chunk_done = true;
                                break;
                            }
                            if (attempt < max_retries)
                                continue;
                            chunk_err = chunk_res->error();
                            chunk_done = true;
                            break;
                        }
                        if (code == 429 || (code >= 500 && code < 600)) {
                            if (attempt < max_retries)
                                continue;
                            chunk_err = cmlb::core::AppError{
                                cmlb::core::ErrorCode::GoogleDriveApi,
                                fmt::format("gdrive: chunk PUT exhausted {} retries: "
                                            "HTTP {}: {}",
                                            max_retries + 1,
                                            code,
                                            chunk_body)};
                            chunk_done = true;
                            break;
                        }
                        chunk_done = true;
                    }
                    body_buffer.clear();

                    if (chunk_err) {
                        bail(std::move(*chunk_err));
                        break;
                    }
                    // Cancellation observed during backoff: exit cleanly without
                    // mis-routing to the "unexpected status code" bail below.
                    if (shared->abort.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (code == 200 || code == 201) {
                        // Final-chunk response carries the file id. Out of
                        // order arrival is fine — whichever chunk Drive
                        // considers last wins this race.
                        try {
                            json doc = json::parse(chunk_body);
                            std::lock_guard lk{shared->fid_mtx};
                            shared->file_id = doc.at("id").get<std::string>();
                        } catch (const std::exception& e) {
                            bail(cmlb::core::AppError{cmlb::core::ErrorCode::JsonParse,
                                                      std::string{"gdrive: final chunk parse: "}
                                                          + e.what()});
                            break;
                        }
                        shared->bytes_uploaded.fetch_add(got, std::memory_order_acq_rel);
                        continue;
                    }
                    if (code != 308) {
                        bail(cmlb::core::AppError{
                            cmlb::core::ErrorCode::GoogleDriveApi,
                            fmt::format("gdrive: chunk PUT returned {}: {}", code, chunk_body)});
                        break;
                    }
                    shared->bytes_uploaded.fetch_add(got, std::memory_order_acq_rel);
                }
            }

            // ExitGuard's destructor handles live_workers decrement + join.
            co_return;
        };

        for (int i = 0; i < parallelism; ++i) {
            // Move a fresh copy into co_spawn — asio takes ownership of the
            // callable, invokes it once to obtain the awaitable, and keeps
            // both alive for the lifetime of the coroutine frame. This
            // avoids the classic "lambda-coroutine with captured this
            // dangles" trap that hits when the factory is destroyed before
            // the coroutine suspends.
            auto factory_copy = worker_factory;
            asio::co_spawn(coro_exec, std::move(factory_copy), asio::detached);
        }

        // Park until the last worker decrements live_workers to zero. We
        // ignore the wait error code — cancel() flips it to operation_aborted
        // which is exactly the "join completed" signal.
        boost::system::error_code wait_ec;
        co_await shared->join_timer.async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));

        if (shared->first_error) {
            co_await abort_resumable_session(session_uri);
            co_return std::unexpected(*shared->first_error);
        }
        {
            std::lock_guard lk{shared->fid_mtx};
            if (shared->file_id.empty()) {
                co_return cmlb::core::error(cmlb::core::ErrorCode::GoogleDriveApi,
                                            "gdrive: no chunk returned final response");
            }
            final_file_id = std::move(shared->file_id);
        }
        // No mid-stream progress emission here — per-chunk completion is
        // out of order so a "bytes_uploaded" callback would be monotonic
        // only by coincidence. The single final callback at function exit
        // is enough for parallel uploads.
    }

    UploadResult out;
    out.file_id = std::move(final_file_id);
    out.link = "https://drive.google.com/file/d/" + out.file_id + "/view";
    out.size = total;
    out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    if (on_progress) {
        UploadProgress p;
        p.file_name = path.filename().string();
        p.total_bytes = total;
        p.uploaded_bytes = total;
        on_progress(p);
    }
    co_return out;
}

boost::asio::awaitable<void> GoogleDriveUploader::abort_resumable_session(std::string session_uri) {
    auto bearer = co_await acquire_bearer();
    HttpRequest req = make_request(HttpMethod::Delete_, session_uri);
    if (bearer) {
        add_header(req, "Authorization", *bearer);
    }
    auto res = co_await http_.request(std::move(req));
    if (!res) {
        cmlb::core::Logger::warn("gdrive: aborting resumable session failed: {}",
                                 res.error().message);
    } else if (res->status_code / 100 != 2 && res->status_code != 499) {
        cmlb::core::Logger::warn("gdrive: aborting resumable session returned {}",
                                 res->status_code);
    }
    co_return;
}

// ----- folder creation ----------------------------------------------------

boost::asio::awaitable<cmlb::core::Result<std::string>> GoogleDriveUploader::create_folder(
    std::string name, std::string parent) {
    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    const json body = {{"name", name},
                       {"mimeType", "application/vnd.google-apps.folder"},
                       {"parents", json::array({parent})}};

    auto req = make_request(HttpMethod::Post, "https://www.googleapis.com/drive/v3/files");
    add_header(req, "Authorization", *bearer);
    add_header(req, "Content-Type", "application/json; charset=UTF-8");
    req.body = body.dump();

    auto res = co_await http_.request(std::move(req));
    if (!res)
        co_return std::unexpected(res.error());
    if (res->status_code / 100 != 2) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::GoogleDriveApi,
            fmt::format("gdrive: create folder returned {}: {}", res->status_code, res->body));
    }
    try {
        json doc = json::parse(res->body);
        co_return doc.at("id").get<std::string>();
    } catch (const std::exception& e) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                    std::string{"gdrive: create folder parse: "} + e.what());
    }
}

// ----- public surface -----------------------------------------------------

boost::asio::awaitable<cmlb::core::Result<UploadResult>> GoogleDriveUploader::upload_file(
    fs::path path, UploadConfig config, UploadProgressHandler on_progress) {
    namespace asio = boost::asio;

    if (!ready_) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "gdrive: uploader not ready (credentials missing)");
    }

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                    "gdrive: file not found: " + path.string());
    }
    const auto size = static_cast<std::int64_t>(fs::file_size(path, ec));
    if (ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                    "gdrive: file_size failed: " + ec.message());
    }

    const std::string parent = config.folder_id.value_or(config_.parent_folder_id);
    if (parent.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "gdrive: no parent folder id (config + override both empty)");
    }

    if ((co_await asio::this_coro::cancellation_state).cancelled()
        != asio::cancellation_type::none) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled, "gdrive: cancelled");
    }

    if (size <= kMultipartThreshold) {
        co_return co_await upload_multipart(std::move(path), parent, std::move(on_progress));
    }
    co_return co_await upload_resumable(std::move(path), parent, std::move(on_progress));
}

boost::asio::awaitable<cmlb::core::Result<std::vector<UploadResult>>>
GoogleDriveUploader::upload_directory(fs::path path,
                                      UploadConfig config,
                                      UploadProgressHandler on_progress) {
    namespace asio = boost::asio;

    if (!ready_) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "gdrive: uploader not ready (credentials missing)");
    }
    std::error_code ec;
    if (!fs::is_directory(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "gdrive: not a directory: " + path.string());
    }
    const std::string root_parent = config.folder_id.value_or(config_.parent_folder_id);
    if (root_parent.empty()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "gdrive: no parent folder id (config + override both empty)");
    }

    // Map relative directory path -> drive folder id. Root entry is the
    // top-level folder we create to mirror `path`.
    std::map<fs::path, std::string> folder_ids;

    auto root_create = co_await create_folder(path.filename().string(), root_parent);
    if (!root_create)
        co_return std::unexpected(root_create.error());
    folder_ids[fs::path{}] = *root_create;

    // Two-pass: walk the tree once to (a) create every folder so the id map
    // is fully populated, and (b) accumulate the per-file work items. Folder
    // creation has to stay sequential because each step depends on the
    // parent's id; file uploads do not, so they fan out below.
    struct FileWork {
        fs::path source;
        std::string parent_id;
    };

    std::vector<FileWork> work;

    for (auto it = fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                        "gdrive: directory walk failed: " + ec.message());
        }
        if ((co_await asio::this_coro::cancellation_state).cancelled()
            != asio::cancellation_type::none) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled, "gdrive: cancelled");
        }
        const fs::path rel = fs::relative(it->path(), path, ec);
        if (ec) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                        "gdrive: relative() failed: " + ec.message());
        }
        if (it->is_directory(ec)) {
            const fs::path parent_rel = rel.parent_path();
            auto parent_it = folder_ids.find(parent_rel);
            const std::string& parent_id =
                (parent_it != folder_ids.end()) ? parent_it->second : folder_ids[fs::path{}];
            auto created = co_await create_folder(rel.filename().string(), parent_id);
            if (!created)
                co_return std::unexpected(created.error());
            folder_ids[rel] = *created;
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;

        const fs::path parent_rel = rel.parent_path();
        auto parent_it = folder_ids.find(parent_rel);
        const std::string& parent_id =
            (parent_it != folder_ids.end()) ? parent_it->second : folder_ids[fs::path{}];
        work.push_back(FileWork{it->path(), parent_id});
    }

    const int file_parallelism = std::clamp(
        config_.parallel_files_per_directory, 1, std::max(1, static_cast<int>(work.size())));

    // Bounded worker pool. Each worker pulls the next index, awaits
    // upload_file, and pushes the result under the result mutex. First
    // error wins and aborts siblings via the shared flag. When
    // file_parallelism is 1 or there's a single file in `work` the pool
    // degenerates to a single sequential worker — no separate code path.
    struct DirShared {
        std::atomic<std::size_t> next_index;
        std::atomic<int> live_workers;
        std::atomic<bool> abort;
        std::mutex err_mtx;
        std::optional<cmlb::core::AppError> first_error;
        std::mutex res_mtx;
        std::vector<UploadResult> results;
        asio::steady_timer join_timer;

        DirShared(asio::any_io_executor ex, int workers)
            : next_index{0}, live_workers{workers}, abort{false}, join_timer{ex} {
            join_timer.expires_at(std::chrono::steady_clock::time_point::max());
        }
    };

    auto coro_exec = co_await asio::this_coro::executor;
    auto shared = std::make_shared<DirShared>(coro_exec, file_parallelism);
    auto work_ptr = std::make_shared<std::vector<FileWork>>(std::move(work));

    auto file_worker =
        [this, shared, work_ptr, config, on_progress]() mutable -> asio::awaitable<void> {
        // Exit guard: decrement live_workers and wake the parent on the
        // last worker out, even if the body throws (cancellation, bad_alloc,
        // upload_file errors that propagate as exceptions, etc.). A leaked
        // decrement leaves the parent coroutine sleeping on join_timer.
        struct ExitGuard {
            std::shared_ptr<DirShared> shared;
            ~ExitGuard() noexcept {
                if (shared->live_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    shared->join_timer.cancel();
                }
            }
        };
        ExitGuard exit_guard{shared};
        while (!shared->abort.load(std::memory_order_acquire)) {
            if ((co_await asio::this_coro::cancellation_state).cancelled()
                != asio::cancellation_type::none) {
                shared->abort.store(true, std::memory_order_release);
                break;
            }
            const std::size_t idx = shared->next_index.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= work_ptr->size())
                break;
            const auto& w = (*work_ptr)[idx];

            UploadConfig per_file = config;
            per_file.folder_id = w.parent_id;
            auto res = co_await upload_file(w.source, per_file, on_progress);
            if (!res) {
                std::lock_guard lk{shared->err_mtx};
                if (!shared->first_error) {
                    shared->first_error = res.error();
                }
                shared->abort.store(true, std::memory_order_release);
                break;
            }
            std::lock_guard lk{shared->res_mtx};
            shared->results.push_back(std::move(*res));
        }
        co_return;
    };

    for (int i = 0; i < file_parallelism; ++i) {
        // Move a fresh copy of the lambda into co_spawn — asio takes
        // ownership of the callable, invokes it to obtain the awaitable,
        // and keeps both alive for the lifetime of the coroutine frame.
        auto worker_copy = file_worker;
        asio::co_spawn(coro_exec, std::move(worker_copy), asio::detached);
    }

    boost::system::error_code wait_ec;
    co_await shared->join_timer.async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));

    if (shared->first_error) {
        co_return std::unexpected(*shared->first_error);
    }
    co_return std::move(shared->results);
}

// ---------------------------------------------------------------------------
// Drive helper API: copy / count / delete used by the application layer.
// ---------------------------------------------------------------------------

namespace {

/// Single-page metadata fetch: returns the Drive metadata blob for @p id with
/// the fields we care about. Status 404 → NotFound; everything else surfaces
/// as GoogleDriveApi.
[[nodiscard]] boost::asio::awaitable<cmlb::core::Result<json>> fetch_metadata(
    BeastHttpClient& http, const std::string& bearer, const std::string& id) {
    auto req = make_request(HttpMethod::Get,
                            fmt::format("https://www.googleapis.com/drive/v3/files/"
                                        "{}?fields=id,name,mimeType,size,parents",
                                        url_escape(id)));
    add_header(req, "Authorization", bearer);

    auto res = co_await http.request(std::move(req));
    if (!res)
        co_return std::unexpected(res.error());
    if (res->status_code == 404) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound, "gdrive: id not found: " + id);
    }
    if (res->status_code / 100 != 2) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::GoogleDriveApi,
            fmt::format(
                "gdrive: metadata fetch {} returned {}: {}", id, res->status_code, res->body));
    }
    try {
        co_return json::parse(res->body);
    } catch (const std::exception& e) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                    std::string{"gdrive: metadata parse: "} + e.what());
    }
}

/// Lists every direct child of @p parent_id. Handles paginated responses
/// (drive's default page size is 100; we ask for 1000 explicitly).
[[nodiscard]] boost::asio::awaitable<cmlb::core::Result<std::vector<json>>> list_children(
    BeastHttpClient& http, const std::string& bearer, const std::string& parent_id) {
    std::vector<json> out;
    std::string page_token;

    while (true) {
        std::string url = fmt::format("https://www.googleapis.com/drive/v3/files?"
                                      "q={}+in+parents+and+trashed%3Dfalse"
                                      "&fields=nextPageToken,files(id,name,mimeType,size)"
                                      "&pageSize=1000",
                                      url_escape("'" + parent_id + "'"));
        if (!page_token.empty()) {
            url += "&pageToken=" + url_escape(page_token);
        }
        auto req = make_request(HttpMethod::Get, std::move(url));
        add_header(req, "Authorization", bearer);

        auto res = co_await http.request(std::move(req));
        if (!res)
            co_return std::unexpected(res.error());
        if (res->status_code / 100 != 2) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::GoogleDriveApi,
                                        fmt::format("gdrive: list children of {} returned {}: {}",
                                                    parent_id,
                                                    res->status_code,
                                                    res->body));
        }
        try {
            json doc = json::parse(res->body);
            if (doc.contains("files") && doc["files"].is_array()) {
                for (const auto& f : doc["files"]) {
                    out.push_back(f);
                }
            }
            if (doc.contains("nextPageToken") && doc["nextPageToken"].is_string()) {
                page_token = doc["nextPageToken"].get<std::string>();
                continue;
            }
        } catch (const std::exception& e) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                        std::string{"gdrive: list children parse: "} + e.what());
        }
        break;
    }
    co_return out;
}

constexpr std::string_view kFolderMime = "application/vnd.google-apps.folder";

} // namespace

boost::asio::awaitable<cmlb::core::Result<std::string>> GoogleDriveUploader::copy(
    std::string source_id, std::string target_folder_id) {
    namespace asio = boost::asio;
    if (!ready_) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "gdrive: uploader not ready");
    }
    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    auto meta = co_await fetch_metadata(http_, *bearer, source_id);
    if (!meta)
        co_return std::unexpected(meta.error());
    const std::string mime = meta->value("mimeType", "");
    const std::string name = meta->value("name", std::string{"copy"});

    // File copy is a single REST call.
    if (mime != kFolderMime) {
        const json body = {{"parents", json::array({target_folder_id})}};
        auto req = make_request(HttpMethod::Post,
                                fmt::format("https://www.googleapis.com/drive/v3/files/{}/copy",
                                            url_escape(source_id)));
        add_header(req, "Authorization", *bearer);
        add_header(req, "Content-Type", "application/json; charset=UTF-8");
        req.body = body.dump();
        auto res = co_await http_.request(std::move(req));
        if (!res)
            co_return std::unexpected(res.error());
        if (res->status_code / 100 != 2) {
            co_return cmlb::core::error(
                cmlb::core::ErrorCode::GoogleDriveApi,
                fmt::format("gdrive: copy returned {}: {}", res->status_code, res->body));
        }
        try {
            co_return json::parse(res->body).at("id").get<std::string>();
        } catch (const std::exception& e) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::JsonParse,
                                        std::string{"gdrive: copy response parse: "} + e.what());
        }
    }

    // Folder copy is recursive — create a sibling folder, then copy each
    // child into it.
    auto root_id = co_await create_folder(name, target_folder_id);
    if (!root_id)
        co_return std::unexpected(root_id.error());

    struct Frame {
        std::string source;
        std::string destination;
    };

    std::vector<Frame> stack;
    stack.push_back(Frame{source_id, *root_id});

    while (!stack.empty()) {
        if ((co_await asio::this_coro::cancellation_state).cancelled()
            != asio::cancellation_type::none) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled, "gdrive: copy cancelled");
        }
        Frame frame = std::move(stack.back());
        stack.pop_back();

        auto children = co_await list_children(http_, *bearer, frame.source);
        if (!children)
            co_return std::unexpected(children.error());

        for (const auto& child : *children) {
            const std::string cid = child.value("id", "");
            if (cid.empty())
                continue;
            const std::string cmime = child.value("mimeType", "");
            const std::string cname = child.value("name", std::string{"item"});

            if (cmime == kFolderMime) {
                auto created = co_await create_folder(cname, frame.destination);
                if (!created)
                    co_return std::unexpected(created.error());
                stack.push_back(Frame{cid, *created});
            } else {
                const json body = {{"parents", json::array({frame.destination})}};
                auto req =
                    make_request(HttpMethod::Post,
                                 fmt::format("https://www.googleapis.com/drive/v3/files/{}/copy",
                                             url_escape(cid)));
                add_header(req, "Authorization", *bearer);
                add_header(req, "Content-Type", "application/json; charset=UTF-8");
                req.body = body.dump();

                auto res = co_await http_.request(std::move(req));
                if (!res)
                    co_return std::unexpected(res.error());
                if (res->status_code / 100 != 2) {
                    co_return cmlb::core::error(cmlb::core::ErrorCode::GoogleDriveApi,
                                                fmt::format("gdrive: child copy returned {}: {}",
                                                            res->status_code,
                                                            res->body));
                }
            }
        }
    }
    co_return *root_id;
}

boost::asio::awaitable<cmlb::core::Result<CountResult>> GoogleDriveUploader::count(
    std::string folder_id) {
    namespace asio = boost::asio;
    if (!ready_) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "gdrive: uploader not ready");
    }
    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    CountResult totals;
    std::vector<std::string> queue;
    queue.push_back(std::move(folder_id));

    while (!queue.empty()) {
        if ((co_await asio::this_coro::cancellation_state).cancelled()
            != asio::cancellation_type::none) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled,
                                        "gdrive: count cancelled");
        }
        const std::string here = std::move(queue.back());
        queue.pop_back();

        auto children = co_await list_children(http_, *bearer, here);
        if (!children)
            co_return std::unexpected(children.error());

        for (const auto& child : *children) {
            const std::string cmime = child.value("mimeType", "");
            if (cmime == kFolderMime) {
                totals.folders += 1;
                const std::string cid = child.value("id", "");
                if (!cid.empty()) {
                    queue.push_back(cid);
                }
            } else {
                totals.files += 1;
                // `size` is returned as a string per Drive API contract.
                if (child.contains("size") && child["size"].is_string()) {
                    try {
                        totals.total_bytes += std::stoll(child["size"].get<std::string>());
                    } catch (...) {
                        // Skip non-integer sizes (e.g. Google docs).
                    }
                }
            }
        }
    }
    co_return totals;
}

boost::asio::awaitable<cmlb::core::Result<void>> GoogleDriveUploader::remove(std::string file_id) {
    if (!ready_) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidState,
                                    "gdrive: uploader not ready");
    }
    auto bearer = co_await acquire_bearer();
    if (!bearer)
        co_return std::unexpected(bearer.error());

    auto req = make_request(
        HttpMethod::Delete_,
        fmt::format("https://www.googleapis.com/drive/v3/files/{}", url_escape(file_id)));
    add_header(req, "Authorization", *bearer);

    auto res = co_await http_.request(std::move(req));
    if (!res)
        co_return std::unexpected(res.error());
    if (res->status_code == 404) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                    "gdrive: file not found: " + file_id);
    }
    if (res->status_code / 100 != 2) {
        co_return cmlb::core::error(
            cmlb::core::ErrorCode::GoogleDriveApi,
            fmt::format("gdrive: delete returned {}: {}", res->status_code, res->body));
    }
    co_return cmlb::core::Result<void>{};
}

} // namespace cmlb::infrastructure::upload
