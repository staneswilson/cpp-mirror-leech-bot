// ---------------------------------------------------------------------------
// leech_url.cpp — LeechUrl use case implementation.
// ---------------------------------------------------------------------------

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fmt/format.h>

#include <cmlb/application/active_task_registry.hpp>
#include <cmlb/application/detail/task_id_generator.hpp>
#include <cmlb/application/leech_url.hpp>
#include <cmlb/core/cancellation.hpp>
#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/task.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace upload_ns = cmlb::infrastructure::upload;
namespace tg_ns = cmlb::infrastructure::telegram;

namespace {

constexpr std::chrono::seconds kPollInterval{3};
constexpr std::chrono::seconds kStatusRpcTimeout{15};
constexpr std::chrono::milliseconds kCoordinationTick{200};

/// Shared coordination state between the poll loop and per-file upload
/// coroutines. The use-case awaits on a steady_timer; spawned uploads
/// update counters under `mu` and the loop polls them every tick.
struct PipelineState {
    std::mutex mu;
    std::unordered_set<std::string> queued;
    int in_flight{0};
    int completed{0};
    int failed{0};
    std::optional<cmlb::core::AppError> first_error;
    std::vector<std::unique_ptr<asio::cancellation_signal>> upload_signals;
};

} // namespace

LeechUrl::LeechUrl(download_ns::DownloaderInterface& aria2_downloader,
                   download_ns::DownloaderInterface& qbit_downloader,
                   upload_ns::UploaderInterface& telegram_uploader,
                   cmlb::infrastructure::persistence::TaskRepository& tasks,
                   cmlb::infrastructure::persistence::UserSettingsRepository& user_settings,
                   tg_ns::MessengerInterface& messenger,
                   cmlb::application::ProgressRendererInterface& progress_renderer,
                   cmlb::core::Executor& executor,
                   cmlb::application::ActiveTaskRegistry& active_tasks,
                   int upload_pool_size) noexcept
    : aria2_{aria2_downloader},
      qbit_{qbit_downloader},
      telegram_uploader_{telegram_uploader},
      tasks_{tasks},
      user_settings_{user_settings},
      messenger_{messenger},
      progress_renderer_{progress_renderer},
      executor_{executor},
      active_tasks_{active_tasks},
      upload_pool_size_{upload_pool_size > 0 ? upload_pool_size : 1} {
}

asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> LeechUrl::execute(LeechRequest request) {
    cmlb::core::Logger::info("leech_url: starting user={} chat={} backend={} url={}",
                             request.user.value(),
                             request.chat.value(),
                             request.use_qbittorrent ? "qbittorrent" : "aria2",
                             request.url);

    if (request.url.empty()) {
        cmlb::core::Logger::warn("leech_url: empty URL rejected");
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument, "leech: empty URL");
    }

    auto settings_res = co_await user_settings_.get(request.user);
    if (!settings_res)
        co_return std::unexpected(settings_res.error());

    auto& downloader = request.use_qbittorrent ? qbit_ : aria2_;

    // Pipeline uploads alongside the download when the backend reports
    // per-file completion incrementally. aria2 opts in; qBittorrent does
    // not (torrents must finish + seed before any Telegram traffic).
    const bool upload_while_downloading = downloader.supports_pipelining();

    auto status_msg = co_await messenger_.send_html(
        request.chat,
        fmt::format("<b>Queued (leech)</b>: <code>{}</code>",
                    cmlb::core::escape_html(cmlb::core::truncate_for_display(request.url, 200))));
    if (!status_msg)
        co_return std::unexpected(status_msg.error());

    const auto now = std::chrono::system_clock::now();
    cmlb::domain::TaskMetadata meta{
        .id = detail::make_task_id(),
        .user = request.user,
        .chat = request.chat,
        .status_message = *status_msg,
        .kind = cmlb::domain::TaskKind::Leech,
        .source_url = request.url,
        .created_at = now,
        .updated_at = now,
    };
    cmlb::domain::Task task{meta};

    {
        auto saved = co_await tasks_.save(task);
        if (!saved)
            co_return std::unexpected(saved.error());
    }

    // Register with the active-task registry so `/cancel` can signal us.
    // Auto-unregisters on every exit path via RAII.
    ActiveTaskGuard active_guard{active_tasks_, meta.id};

    // Best-effort persistence helper — see mirror_url.cpp for rationale.
    auto persist_best_effort =
        [&, task_id = meta.id.value()](std::string_view stage) -> asio::awaitable<void> {
        auto saved = co_await tasks_.save(task);
        if (!saved) {
            cmlb::core::Logger::warn("leech_url: task={} persist after {} failed: {}",
                                     task_id,
                                     stage,
                                     saved.error().message);
        }
        co_return;
    };

    auto fail_task =
        [&](std::string_view stage,
            cmlb::core::AppError err) -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        cmlb::core::Logger::error("leech_url: failed task={} stage={} code={} msg={}",
                                  meta.id.value(),
                                  stage,
                                  cmlb::core::error_code_name(err.code),
                                  err.message);
        (void)task.mark_failed(err.message);
        co_await persist_best_effort("mark_failed");
        const std::string html = fmt::format(
            "<b>Failed</b> ({})\n"
            "<b>Reason:</b> {}\n"
            "<pre>{}</pre>",
            cmlb::core::escape_html(stage),
            cmlb::core::escape_html(cmlb::core::friendly_error_label(err.code)),
            cmlb::core::escape_html(cmlb::core::truncate_for_display(err.message, 512)));
        (void)co_await messenger_.edit_html(request.chat, *status_msg, html);
        co_return std::unexpected(std::move(err));
    };
    auto fail_with = [&](std::string_view stage, cmlb::core::ErrorCode code, std::string message)
        -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await fail_task(stage, cmlb::core::AppError{code, std::move(message)});
    };

    if (auto t = task.start_download(); !t) {
        co_return co_await fail_task("state", t.error());
    }
    co_await persist_best_effort("start_download");

    download_ns::DownloadOptions opts{};
    auto gid_res = co_await downloader.add_uri(request.url, opts);
    if (!gid_res) {
        co_return co_await fail_task("download", gid_res.error());
    }
    const auto gid = *gid_res;
    const auto downloader_kind = request.use_qbittorrent ? cmlb::domain::DownloaderKind::Qbittorrent
                                                         : cmlb::domain::DownloaderKind::Aria2;
    task.attach_downloader(gid, downloader_kind);
    co_await persist_best_effort("attach_downloader");
    cmlb::core::Logger::info("leech_url: task={} gid={} backend={} downloading",
                             meta.id.value(),
                             gid.value(),
                             cmlb::domain::to_string(downloader_kind));

    // ---- Build upload config (shared across spawned uploads) ------------
    upload_ns::UploadConfig up_cfg{};
    up_cfg.chat_id = request.chat;
    if (settings_res->has_value()) {
        const auto& s = **settings_res;
        up_cfg.as_document = s.upload_as_document;
        if (s.default_thumb_path) {
            up_cfg.thumbnail_path = *s.default_thumb_path;
        }
    }

    auto coro_exec = co_await asio::this_coro::executor;
    asio::steady_timer timer{coro_exec};
    download_ns::DownloadStatus final_status;
    auto state = std::make_shared<PipelineState>();
    bool transitioned_to_uploading = false;

    upload_ns::UploaderInterface* uploader_ptr = &telegram_uploader_;
    auto spawn_upload = [&, uploader_ptr](std::filesystem::path file_path) {
        {
            std::lock_guard lk{state->mu};
            if (!state->queued.insert(file_path.string()).second) {
                return;
            }
            ++state->in_flight;
            state->upload_signals.push_back(std::make_unique<asio::cancellation_signal>());
        }
        auto signal_slot = state->upload_signals.back()->slot();
        asio::co_spawn(
            executor_.get_executor(),
            [state, uploader_ptr, file_path, up_cfg]() -> asio::awaitable<void> {
                auto res = co_await uploader_ptr->upload_file(file_path, up_cfg, {});
                {
                    std::lock_guard lk{state->mu};
                    --state->in_flight;
                    if (!res) {
                        ++state->failed;
                        if (!state->first_error) {
                            state->first_error = res.error();
                        }
                    } else {
                        ++state->completed;
                    }
                }
                co_return;
            },
            asio::bind_cancellation_slot(signal_slot, asio::detached));
    };

    auto cancel_all_uploads = [&]() {
        std::lock_guard lk{state->mu};
        for (auto& sig : state->upload_signals) {
            if (sig)
                sig->emit(asio::cancellation_type::all);
        }
    };

    auto drain_uploads = [&]() -> asio::awaitable<void> {
        asio::steady_timer drain_timer{coro_exec};
        while (true) {
            {
                std::lock_guard lk{state->mu};
                if (state->in_flight == 0)
                    co_return;
            }
            drain_timer.expires_after(kCoordinationTick);
            co_await drain_timer.async_wait(asio::use_awaitable);
        }
    };

    auto can_spawn_more = [&]() -> bool {
        std::lock_guard lk{state->mu};
        return state->in_flight < upload_pool_size_;
    };

    bool download_complete = false;
    while (true) {
        const auto cs = co_await asio::this_coro::cancellation_state;
        const bool coroutine_cancelled = cs.cancelled() != asio::cancellation_type::none;
        const bool registry_cancelled = active_guard.cancelled();
        if (coroutine_cancelled || registry_cancelled) {
            cancel_all_uploads();
            co_await drain_uploads();
            (void)co_await downloader.remove(gid, true);
            (void)task.mark_cancelled();
            co_await persist_best_effort("mark_cancelled");
            (void)co_await messenger_.edit_html(request.chat, *status_msg, "<b>Cancelled</b>");
            cmlb::core::Logger::info("leech_url: task={} cancelled (source={})",
                                     meta.id.value(),
                                     registry_cancelled ? "registry" : "coroutine");
            co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled,
                                        "leech: cancelled by user");
        }

        if (!download_complete) {
            auto status =
                co_await cmlb::core::with_timeout(downloader.status(gid), kStatusRpcTimeout);
            if (!status) {
                cancel_all_uploads();
                co_await drain_uploads();
                co_return co_await fail_task("download", status.error());
            }
            final_status = *status;

            if (final_status.state == download_ns::DownloadState::Error) {
                cancel_all_uploads();
                co_await drain_uploads();
                const std::string msg =
                    final_status.error_message.value_or("downloader reported error");
                co_return co_await fail_with("download", cmlb::core::ErrorCode::Aria2Rpc, msg);
            }
            if (final_status.state == download_ns::DownloadState::Removed) {
                cancel_all_uploads();
                co_await drain_uploads();
                co_return co_await fail_with(
                    "download", cmlb::core::ErrorCode::Cancelled, "download removed externally");
            }
            // Treat `Seeding` as "files are on disk, upload-ready" — qBit
            // never emits `Complete` (it transitions Downloading →
            // uploading/stalledUP/forcedUP, all mapped to Seeding). Without
            // this, torrent leeches would never exit the poll loop.
            if (final_status.state == download_ns::DownloadState::Complete
                || final_status.state == download_ns::DownloadState::Seeding) {
                download_complete = true;
            }

            if (upload_while_downloading) {
                for (const auto& file : final_status.files) {
                    if (!can_spawn_more())
                        break;
                    bool already;
                    {
                        std::lock_guard lk{state->mu};
                        already = state->queued.contains(file.string());
                    }
                    if (already)
                        continue;
                    if (!transitioned_to_uploading) {
                        if (auto t = task.begin_upload(); t) {
                            transitioned_to_uploading = true;
                            co_await persist_best_effort("begin_upload");
                        }
                    }
                    spawn_upload(file);
                }
            }

            std::array<download_ns::DownloadStatus, 1> active{final_status};
            (void)co_await progress_renderer_.render(
                request.chat, std::span<const download_ns::DownloadStatus>{active});
        }

        bool all_uploads_done;
        bool any_upload_failed;
        {
            std::lock_guard lk{state->mu};
            all_uploads_done = (state->in_flight == 0);
            any_upload_failed = state->failed > 0;
        }

        if (any_upload_failed) {
            cancel_all_uploads();
            co_await drain_uploads();
            cmlb::core::AppError upload_err{cmlb::core::ErrorCode::Unknown, "upload failed"};
            {
                std::lock_guard lk{state->mu};
                if (state->first_error) {
                    upload_err = *state->first_error;
                }
            }
            (void)co_await downloader.remove(gid, false);
            co_return co_await fail_task("upload", std::move(upload_err));
        }

        if (download_complete && all_uploads_done) {
            break;
        }

        timer.expires_after(download_complete ? kCoordinationTick : kPollInterval);
        co_await timer.async_wait(asio::use_awaitable);
    }

    // ---- Fallback for non-pipelined path --------------------------------
    int completed_count;
    {
        std::lock_guard lk{state->mu};
        completed_count = state->completed;
    }

    if (completed_count == 0) {
        if (auto t = task.begin_upload(); !t) {
            co_return co_await fail_task("state", t.error());
        }
        co_await persist_best_effort("begin_upload_fallback");
        (void)co_await messenger_.edit_html(
            request.chat, *status_msg, "<b>Uploading</b> to Telegram");

        if (final_status.files.empty()) {
            co_return co_await fail_with("download",
                                         cmlb::core::ErrorCode::Internal,
                                         "downloader reported no produced files");
        }
        if (final_status.save_path) {
            auto dir_res =
                co_await telegram_uploader_.upload_directory(*final_status.save_path, up_cfg, {});
            if (!dir_res) {
                co_return co_await fail_task("upload", dir_res.error());
            }
            if (dir_res->empty()) {
                co_return co_await fail_with(
                    "upload", cmlb::core::ErrorCode::Internal, "upload produced no results");
            }
        } else {
            auto file_res =
                co_await telegram_uploader_.upload_file(final_status.files.front(), up_cfg, {});
            if (!file_res) {
                co_return co_await fail_task("upload", file_res.error());
            }
        }
    }

    if (!transitioned_to_uploading) {
        if (auto t = task.begin_upload(); !t) {
            // best-effort
        }
    }
    if (auto t = task.mark_completed(); !t) {
        co_return co_await fail_task("state", t.error());
    }
    co_await persist_best_effort("mark_completed");
    // Release downloader-side resources (stops qBit seeding, removes the
    // aria2 entry from the active list). `delete_files=false` to keep the
    // on-disk artefacts in case the operator wants them around.
    (void)co_await downloader.remove(gid, false);
    (void)co_await messenger_.edit_html(request.chat, *status_msg, "<b>Completed</b>");
    cmlb::core::Logger::info("leech_url: task={} completed", meta.id.value());
    co_return meta.id;
}

} // namespace cmlb::application
