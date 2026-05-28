#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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

#include <fmt/format.h>

#include <cmlb/core/error.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/identifiers.hpp>
#include <cmlb/infrastructure/telegram/messenger.hpp>
#include <cmlb/infrastructure/upload/telegram_uploader.hpp>

namespace cmlb::infrastructure::upload {

namespace {

namespace fs = std::filesystem;

/// 1 MiB streaming buffer for `split_file` — keeps RAM bounded regardless of
/// the original file size.
constexpr std::size_t kSplitStreamBuffer = 1024 * 1024;

/// Lowercases an ASCII extension (including the leading dot) for table lookup.
[[nodiscard]] std::string lower_ext(const fs::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

enum class MediaKind {
    Document,
    Video,
    Audio,
    Photo
};

[[nodiscard]] std::string_view kind_label(MediaKind k) noexcept {
    switch (k) {
    case MediaKind::Video:
        return "video";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Photo:
        return "photo";
    case MediaKind::Document:
        return "document";
    }
    return "document";
}

[[nodiscard]] MediaKind classify(const fs::path& p) {
    static constexpr std::array<std::string_view, 5> video{".mp4", ".mkv", ".mov", ".avi", ".webm"};
    static constexpr std::array<std::string_view, 6> audio{
        ".mp3", ".flac", ".wav", ".opus", ".m4a", ".aac"};
    static constexpr std::array<std::string_view, 4> photo{".jpg", ".jpeg", ".png", ".webp"};

    const std::string ext = lower_ext(p);
    if (std::ranges::find(video, ext) != video.end())
        return MediaKind::Video;
    if (std::ranges::find(audio, ext) != audio.end())
        return MediaKind::Audio;
    if (std::ranges::find(photo, ext) != photo.end())
        return MediaKind::Photo;
    return MediaKind::Document;
}

/// Splits `src` into `partNNN`-suffixed files of `chunk_bytes` each. Streams
/// in 1 MiB buffers. Returns the produced chunk paths.
[[nodiscard]] cmlb::core::Result<std::vector<fs::path>> split_file(const fs::path& src,
                                                                   std::int64_t chunk_bytes) {
    std::ifstream in{src, std::ios::binary};
    if (!in) {
        return cmlb::core::error(cmlb::core::ErrorCode::Io,
                                 "telegram_uploader: cannot open " + src.string()
                                     + " for splitting");
    }

    std::error_code ec;
    const std::uintmax_t total = fs::file_size(src, ec);
    if (ec) {
        return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                 "telegram_uploader: file_size failed: " + ec.message());
    }
    if (chunk_bytes <= 0) {
        return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                 "telegram_uploader: split_size must be > 0");
    }

    const auto chunk_count = static_cast<int>((total + static_cast<std::uintmax_t>(chunk_bytes) - 1)
                                              / static_cast<std::uintmax_t>(chunk_bytes));

    std::vector<fs::path> parts;
    parts.reserve(static_cast<std::size_t>(chunk_count));

    std::vector<char> buffer(kSplitStreamBuffer);
    for (int i = 0; i < chunk_count; ++i) {
        fs::path part_path = src.string() + fmt::format(".part{:03d}", i + 1);
        std::ofstream out{part_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return cmlb::core::error(cmlb::core::ErrorCode::Io,
                                     "telegram_uploader: cannot create part " + part_path.string());
        }
        std::int64_t remaining = chunk_bytes;
        while (remaining > 0 && in) {
            const auto to_read = static_cast<std::streamsize>(
                std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size())));
            in.read(buffer.data(), to_read);
            const auto got = in.gcount();
            if (got <= 0)
                break;
            out.write(buffer.data(), got);
            if (!out) {
                return cmlb::core::error(cmlb::core::ErrorCode::Io,
                                         "telegram_uploader: write failed for "
                                             + part_path.string());
            }
            remaining -= got;
        }
        parts.push_back(std::move(part_path));
    }

    return parts;
}

} // namespace

// --------------------------------------------------------------------------
// UploadProgress::progress_percent  (lives here to keep the header lean)
// --------------------------------------------------------------------------

double UploadProgress::progress_percent() const noexcept {
    if (total_bytes <= 0)
        return 0.0;
    const double pct = static_cast<double>(uploaded_bytes) / static_cast<double>(total_bytes);
    if (pct < 0.0)
        return 0.0;
    if (pct > 1.0)
        return 1.0;
    return pct;
}

// --------------------------------------------------------------------------
// TelegramUploader
// --------------------------------------------------------------------------

TelegramUploader::TelegramUploader(cmlb::infrastructure::telegram::MessengerInterface& messenger,
                                   const cmlb::core::TelegramConfig& telegram_config) noexcept
    : messenger_{messenger}, telegram_config_{telegram_config} {
}

boost::asio::awaitable<cmlb::core::Result<UploadResult>> TelegramUploader::upload_file(
    fs::path path, UploadConfig config, UploadProgressHandler on_progress) {
    namespace asio = boost::asio;
    const auto started = std::chrono::steady_clock::now();

    if (!config.chat_id.has_value()) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "telegram_uploader: UploadConfig.chat_id is required");
    }
    const auto chat = *config.chat_id;

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::NotFound,
                                    "telegram_uploader: file not found: " + path.string());
    }
    const auto raw_size = fs::file_size(path, ec);
    if (ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                    "telegram_uploader: file_size failed: " + ec.message());
    }
    const auto file_size = static_cast<std::int64_t>(raw_size);
    const auto split_bytes = config.split_size.bytes();
    const auto display_name = path.filename().string();

    // Honor caller cancellation before we touch the network.
    if ((co_await asio::this_coro::cancellation_state).cancelled()
        != asio::cancellation_type::none) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled,
                                    "telegram_uploader: cancelled");
    }

    const MediaKind kind = config.as_document ? MediaKind::Document : classify(path);
    cmlb::core::Logger::debug("telegram_uploader: dispatch {} ({} bytes, kind={})",
                              display_name,
                              file_size,
                              kind_label(kind));

    if (file_size <= split_bytes) {
        UploadProgress prog;
        prog.file_name = display_name;
        prog.total_bytes = file_size;
        prog.uploaded_bytes = 0;
        prog.current_part = 1;
        prog.total_parts = 1;
        if (on_progress)
            on_progress(prog);

        const std::string caption = config.caption.value_or(std::string{});
        auto msg_res = co_await messenger_.send_file(chat, path, caption, config.thumbnail_path);
        if (!msg_res)
            co_return std::unexpected(msg_res.error());

        prog.uploaded_bytes = file_size;
        if (on_progress)
            on_progress(prog);

        UploadResult out;
        out.file_id = std::to_string(msg_res->value());
        out.size = file_size;
        out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        co_return out;
    }

    auto parts_res = split_file(path, split_bytes);
    if (!parts_res)
        co_return std::unexpected(parts_res.error());
    auto& parts = *parts_res;
    const int total_parts = static_cast<int>(parts.size());

    UploadResult agg;
    agg.size = 0;

    // Bounded worker pool sized by `upload_parallelism`. When the value is
    // 1 (or `total_parts == 1`), `parallelism == 1` and the pool degenerates
    // to a single sequential worker — no separate serial code path.
    const int configured_parallelism = telegram_config_.upload_parallelism;
    const int parallelism = std::min(std::max(configured_parallelism, 1), total_parts);

    {
        // Detached workers may outlive any early-return path on this awaitable
        // (e.g. cancellation tearing down the join wait), so every byte they
        // touch — paths, results, the join timer — lives on the heap behind
        // a shared_ptr that the workers themselves keep alive.
        struct SharedState {
            std::vector<fs::path> part_paths;
            std::atomic<int> next_index{0};
            std::atomic<bool> abort{false};
            std::atomic<int> live{0};
            std::atomic<std::int64_t> uploaded_bytes{0};
            std::mutex error_mu;
            std::optional<cmlb::core::AppError> first_error;
            std::vector<std::optional<cmlb::domain::MessageId>> results;
            std::vector<std::int64_t> part_sizes;
            std::mutex progress_mu;
            std::chrono::steady_clock::time_point last_cb;
            std::unique_ptr<asio::steady_timer> done_timer;
        };

        auto coro_exec = co_await asio::this_coro::executor;
        auto shared = std::make_shared<SharedState>();
        shared->part_paths = parts;
        shared->results.resize(static_cast<std::size_t>(total_parts));
        shared->part_sizes.assign(static_cast<std::size_t>(total_parts), 0);
        shared->live.store(parallelism);
        shared->last_cb = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        shared->done_timer = std::make_unique<asio::steady_timer>(coro_exec);
        shared->done_timer->expires_at(std::chrono::steady_clock::time_point::max());

        auto record_error = [shared](cmlb::core::AppError err) {
            std::lock_guard lock{shared->error_mu};
            if (!shared->first_error) {
                shared->first_error = std::move(err);
            }
            shared->abort.store(true, std::memory_order_release);
        };

        for (int w = 0; w < parallelism; ++w) {
            asio::co_spawn(
                coro_exec,
                [shared,
                 &messenger = messenger_,
                 chat,
                 total_parts,
                 file_size,
                 display_name,
                 thumb = config.thumbnail_path,
                 on_progress,
                 record_error]() -> asio::awaitable<void> {
                    struct ExitGuard {
                        std::shared_ptr<SharedState> shared;
                        ~ExitGuard() noexcept {
                            if (shared->live.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                shared->done_timer->cancel();
                            }
                        }
                    };
                    ExitGuard exit_guard{shared};
                    for (;;) {
                        if (shared->abort.load(std::memory_order_acquire))
                            break;

                        if ((co_await asio::this_coro::cancellation_state).cancelled()
                            != asio::cancellation_type::none) {
                            record_error(cmlb::core::AppError{cmlb::core::ErrorCode::Cancelled,
                                                              "telegram_uploader: cancelled"});
                            break;
                        }

                        const int idx = shared->next_index.fetch_add(1, std::memory_order_acq_rel);
                        if (idx >= total_parts)
                            break;

                        const auto& part = shared->part_paths[static_cast<std::size_t>(idx)];
                        std::error_code fec;
                        const auto part_size = static_cast<std::int64_t>(fs::file_size(part, fec));
                        if (fec) {
                            record_error(cmlb::core::AppError{
                                cmlb::core::ErrorCode::FileSystem,
                                "telegram_uploader: file_size on part failed: " + fec.message()});
                            break;
                        }
                        shared->part_sizes[static_cast<std::size_t>(idx)] = part_size;

                        const auto caption =
                            fmt::format("Part{:02d}/{:02d} {}", idx + 1, total_parts, display_name);

                        auto msg_res = co_await messenger.send_file(chat, part, caption, thumb);
                        if (!msg_res) {
                            record_error(msg_res.error());
                            break;
                        }
                        shared->results[static_cast<std::size_t>(idx)] = *msg_res;

                        const auto running =
                            shared->uploaded_bytes.fetch_add(part_size, std::memory_order_acq_rel)
                            + part_size;

                        if (on_progress) {
                            const auto now = std::chrono::steady_clock::now();
                            std::unique_lock prog_lock{shared->progress_mu};
                            if (now - shared->last_cb >= std::chrono::seconds(1)) {
                                shared->last_cb = now;
                                prog_lock.unlock();
                                UploadProgress prog;
                                prog.file_name = display_name;
                                prog.total_bytes = file_size;
                                prog.uploaded_bytes = running;
                                prog.current_part = idx + 1;
                                prog.total_parts = total_parts;
                                on_progress(prog);
                            }
                        }
                    }

                    co_return;
                },
                asio::detached);
        }

        struct PartsCleanup {
            const std::vector<fs::path>* parts;
            bool armed{true};

            ~PartsCleanup() noexcept {
                if (!armed || !parts)
                    return;
                for (auto& p : *parts) {
                    std::error_code ec;
                    fs::remove(p, ec);
                    if (ec) {
                        cmlb::core::Logger::warn("telegram_uploader: failed to remove split part "
                                                 "{}: {}",
                                                 p.string(),
                                                 ec.message());
                    }
                }
            }
        };

        PartsCleanup parts_cleanup{&parts, true};

        boost::system::error_code wait_ec;
        co_await shared->done_timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));

        if (shared->first_error) {
            co_return std::unexpected(*shared->first_error);
        }

        for (int i = 0; i < total_parts; ++i) {
            const auto& slot = shared->results[static_cast<std::size_t>(i)];
            if (!slot) {
                co_return cmlb::core::error(cmlb::core::ErrorCode::Internal,
                                            "telegram_uploader: missing result for part "
                                                + std::to_string(i + 1));
            }
            agg.size += shared->part_sizes[static_cast<std::size_t>(i)];
            if (agg.file_id.empty()) {
                agg.file_id = std::to_string(slot->value());
            }
        }

        if (on_progress) {
            UploadProgress prog;
            prog.file_name = display_name;
            prog.total_bytes = file_size;
            prog.uploaded_bytes = agg.size;
            prog.current_part = total_parts;
            prog.total_parts = total_parts;
            on_progress(prog);
        }
    }
    // PartsCleanup in the inner scope removes the split files on any exit path.

    agg.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    co_return agg;
}

boost::asio::awaitable<cmlb::core::Result<std::vector<UploadResult>>>
TelegramUploader::upload_directory(fs::path path,
                                   UploadConfig config,
                                   UploadProgressHandler on_progress) {
    namespace asio = boost::asio;

    std::error_code ec;
    if (!fs::is_directory(path, ec) || ec) {
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument,
                                    "telegram_uploader: not a directory: " + path.string());
    }

    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            co_return cmlb::core::error(cmlb::core::ErrorCode::FileSystem,
                                        "telegram_uploader: directory walk failed: "
                                            + ec.message());
        }
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    }
    std::ranges::sort(files);

    if (files.empty())
        co_return std::vector<UploadResult>{};

    const int file_parallelism =
        std::clamp(telegram_config_.upload_files_parallelism, 1, static_cast<int>(files.size()));

    // Bounded worker pool. TDLib pipelines its own upload sessions, so
    // multiple concurrent `send_file` calls saturate the link more than a
    // strict for-each loop. With file_parallelism == 1 the pool degenerates
    // to a single sequential worker — no separate code path.
    struct DirShared {
        std::vector<fs::path> files;
        std::atomic<std::size_t> next_index{0};
        std::atomic<int> live_workers{0};
        std::atomic<bool> abort{false};
        std::mutex err_mtx;
        std::optional<cmlb::core::AppError> first_error;
        std::mutex res_mtx;
        std::vector<UploadResult> results;
        std::unique_ptr<asio::steady_timer> join_timer;
    };

    auto coro_exec = co_await asio::this_coro::executor;
    auto shared = std::make_shared<DirShared>();
    shared->files = std::move(files);
    shared->live_workers.store(file_parallelism);
    shared->join_timer = std::make_unique<asio::steady_timer>(coro_exec);
    shared->join_timer->expires_at(std::chrono::steady_clock::time_point::max());

    auto record_error = [shared](cmlb::core::AppError err) {
        std::lock_guard lk{shared->err_mtx};
        if (!shared->first_error)
            shared->first_error = std::move(err);
        shared->abort.store(true, std::memory_order_release);
    };

    for (int w = 0; w < file_parallelism; ++w) {
        asio::co_spawn(
            coro_exec,
            [this, shared, config, on_progress, record_error]() -> asio::awaitable<void> {
                struct ExitGuard {
                    std::shared_ptr<DirShared> shared;
                    ~ExitGuard() noexcept {
                        if (shared->live_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            shared->join_timer->cancel();
                        }
                    }
                };
                ExitGuard exit_guard{shared};
                while (!shared->abort.load(std::memory_order_acquire)) {
                    if ((co_await asio::this_coro::cancellation_state).cancelled()
                        != asio::cancellation_type::none) {
                        record_error(cmlb::core::AppError{cmlb::core::ErrorCode::Cancelled,
                                                          "telegram_uploader: cancelled"});
                        break;
                    }
                    const std::size_t idx =
                        shared->next_index.fetch_add(1, std::memory_order_acq_rel);
                    if (idx >= shared->files.size())
                        break;
                    auto res = co_await upload_file(shared->files[idx], config, on_progress);
                    if (!res) {
                        record_error(res.error());
                        break;
                    }
                    std::lock_guard lk{shared->res_mtx};
                    shared->results.push_back(std::move(*res));
                }
                co_return;
            },
            asio::detached);
    }

    boost::system::error_code wait_ec;
    co_await shared->join_timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_ec));

    if (shared->first_error) {
        co_return std::unexpected(*shared->first_error);
    }
    co_return std::move(shared->results);
}

} // namespace cmlb::infrastructure::upload
