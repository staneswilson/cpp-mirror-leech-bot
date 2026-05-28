// ---------------------------------------------------------------------------
// mirror_url.cpp — MirrorUrl use case implementation.
// ---------------------------------------------------------------------------

#include <array>
#include <chrono>
#include <cstdint>
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
#include <cmlb/application/mirror_url.hpp>
#include <cmlb/core/cancellation.hpp>
#include <cmlb/core/formatting.hpp>
#include <cmlb/core/logger.hpp>
#include <cmlb/domain/task.hpp>
#include <cmlb/domain/upload_destination.hpp>

namespace cmlb::application {

namespace asio = boost::asio;
namespace download_ns = cmlb::infrastructure::download;
namespace upload_ns = cmlb::infrastructure::upload;
namespace tg_ns = cmlb::infrastructure::telegram;

namespace {

constexpr std::chrono::seconds kPollInterval{3};
constexpr std::chrono::seconds kStatusRpcTimeout{15};
constexpr std::chrono::milliseconds kCoordinationTick{200};

[[nodiscard]] cmlb::core::Result<cmlb::domain::UploadDestination> resolve_destination(
    const MirrorRequest& req,
    const std::optional<cmlb::infrastructure::persistence::UserSettingsRecord>& settings) {
    if (req.override_destination) {
        return *req.override_destination;
    }
    if (settings) {
        const auto dest = settings->mirror_destination;
        if (dest == cmlb::domain::UploadDestination::GoogleDrive
            || dest == cmlb::domain::UploadDestination::Rclone) {
            return dest;
        }
    }
    return cmlb::domain::UploadDestination::GoogleDrive;
}

/// Coordination state shared between the poll loop and the spawned
/// per-file upload coroutines. Access is guarded by `mu`; the use-case
/// coroutine waits on a short timer rather than a condition variable so
/// it can keep refreshing the status message between upload completions.
struct PipelineState {
    std::mutex mu;
    std::unordered_set<std::string> queued; // absolute paths
    int in_flight{0};
    int completed{0};
    int failed{0};
    std::optional<cmlb::core::AppError> first_error;
    std::vector<upload_ns::UploadResult> results;
    std::vector<std::unique_ptr<asio::cancellation_signal>> upload_signals;
};

} // namespace

MirrorUrl::MirrorUrl(download_ns::DownloaderInterface& aria2_downloader,
                     download_ns::DownloaderInterface& qbit_downloader,
                     upload_ns::UploaderInterface& gdrive_uploader,
                     upload_ns::UploaderInterface& rclone_uploader,
                     cmlb::infrastructure::persistence::TaskRepository& tasks,
                     cmlb::infrastructure::persistence::UserSettingsRepository& user_settings,
                     tg_ns::MessengerInterface& messenger,
                     cmlb::application::ProgressRendererInterface& progress_renderer,
                     cmlb::core::Executor& executor,
                     cmlb::application::ActiveTaskRegistry& active_tasks,
                     int upload_pool_size) noexcept
    : aria2_{aria2_downloader},
      qbit_{qbit_downloader},
      gdrive_{gdrive_uploader},
      rclone_{rclone_uploader},
      tasks_{tasks},
      user_settings_{user_settings},
      messenger_{messenger},
      progress_renderer_{progress_renderer},
      executor_{executor},
      active_tasks_{active_tasks},
      upload_pool_size_{upload_pool_size > 0 ? upload_pool_size : 1} {
}

asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> MirrorUrl::execute(
    MirrorRequest request) {
    cmlb::core::Logger::info("mirror_url: starting user={} chat={} backend={} url={}",
                             request.user.value(),
                             request.chat.value(),
                             request.use_qbittorrent ? "qbittorrent" : "aria2",
                             request.url);

    if (request.url.empty()) {
        cmlb::core::Logger::warn("mirror_url: empty URL rejected");
        co_return cmlb::core::error(cmlb::core::ErrorCode::InvalidArgument, "mirror: empty URL");
    }

    // ---- Resolve upload destination from user settings -------------------
    auto settings_res = co_await user_settings_.get(request.user);
    if (!settings_res)
        co_return std::unexpected(settings_res.error());

    auto dest_res = resolve_destination(request, *settings_res);
    if (!dest_res)
        co_return std::unexpected(dest_res.error());
    const auto destination = *dest_res;

    auto& downloader = request.use_qbittorrent ? qbit_ : aria2_;
    auto& uploader = (destination == cmlb::domain::UploadDestination::Rclone) ? rclone_ : gdrive_;

    // Pipeline uploads alongside the download when the backend reports
    // per-file completion incrementally. aria2 opts in; qBittorrent does
    // not — its `Complete` state typically transitions straight into
    // `Seeding`, and the operator may want to keep seeding before any
    // upload starts.
    const bool upload_while_downloading = downloader.supports_pipelining();

    // ---- Send the initial status message --------------------------------
    // URL is user-supplied: HTML-escape and bound the display width so a
    // pathological URL can't produce a runaway or malformed status message.
    auto status_msg = co_await messenger_.send_html(
        request.chat,
        fmt::format("<b>Queued</b>: <code>{}</code>",
                    cmlb::core::escape_html(cmlb::core::truncate_for_display(request.url, 200))));
    if (!status_msg)
        co_return std::unexpected(status_msg.error());

    // ---- Construct and persist the task in Queued -----------------------
    const auto now = std::chrono::system_clock::now();
    cmlb::domain::TaskMetadata meta{
        .id = detail::make_task_id(),
        .user = request.user,
        .chat = request.chat,
        .status_message = *status_msg,
        .kind = cmlb::domain::TaskKind::Mirror,
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

    // Register the task with the active-task registry so `/cancel` can
    // signal us. The guard auto-unregisters on every exit path.
    ActiveTaskGuard active_guard{active_tasks_, meta.id};

    // Best-effort persistence helper: state transitions during normal
    // operation are not worth aborting on, but a save failure points at
    // a database problem operators should see in the log.
    auto persist_best_effort =
        [&, task_id = meta.id.value()](std::string_view stage) -> asio::awaitable<void> {
        auto saved = co_await tasks_.save(task);
        if (!saved) {
            cmlb::core::Logger::warn("mirror_url: task={} persist after {} failed: {}",
                                     task_id,
                                     stage,
                                     saved.error().message);
        }
        co_return;
    };

    // Failure helper. Logs the *internal* detail at error level (raw,
    // un-truncated, with the failing stage), then edits the chat status
    // message to a *user-facing* block built from the error code's
    // friendly label plus a sanitized, truncated detail. The internal
    // "downloader add_uri: " / "upload: " framing prefixes never reach
    // the chat — `stage` carries that context structurally instead.
    auto fail_task =
        [&](std::string_view stage,
            cmlb::core::AppError err) -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        cmlb::core::Logger::error("mirror_url: failed task={} stage={} code={} msg={}",
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

    // Convenience: wrap a literal reason in an AppError before delegating
    // to `fail_task`. Used by call sites that synthesize the message from
    // domain state (e.g. "download removed externally") rather than from
    // a propagated AppError.
    auto fail_with = [&](std::string_view stage, cmlb::core::ErrorCode code, std::string message)
        -> asio::awaitable<cmlb::core::Result<cmlb::domain::TaskId>> {
        co_return co_await fail_task(stage, cmlb::core::AppError{code, std::move(message)});
    };

    // ---- Transition Downloading ----------------------------------------
    if (auto t = task.start_download(); !t) {
        co_return co_await fail_task("state", t.error());
    }
    co_await persist_best_effort("start_download");

    // ---- Dispatch to the selected downloader ----------------------------
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
    cmlb::core::Logger::info("mirror_url: task={} gid={} backend={} downloading",
                             meta.id.value(),
                             gid.value(),
                             cmlb::domain::to_string(downloader_kind));

    // ---- Build upload config (shared across all spawned upload tasks) ---
    upload_ns::UploadConfig up_cfg{};
    if (settings_res->has_value()) {
        const auto& s = **settings_res;
        if (destination == cmlb::domain::UploadDestination::GoogleDrive && s.gdrive_folder_id) {
            up_cfg.folder_id = *s.gdrive_folder_id;
        }
        if (destination == cmlb::domain::UploadDestination::Rclone && s.rclone_remote) {
            up_cfg.rclone_path = *s.rclone_remote;
        }
    }

    // ---- Poll loop with download->upload pipelining ---------------------
    auto coro_exec = co_await asio::this_coro::executor;
    asio::steady_timer timer{coro_exec};
    download_ns::DownloadStatus final_status;
    auto state = std::make_shared<PipelineState>();
    bool transitioned_to_uploading = false;

    upload_ns::UploaderInterface* uploader_ptr = &uploader;
    auto spawn_upload = [&, uploader_ptr](std::filesystem::path file_path) {
        {
            std::lock_guard lk{state->mu};
            if (!state->queued.insert(file_path.string()).second) {
                return; // already queued
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
                        state->results.push_back(std::move(*res));
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

    // Block the use-case coroutine until every spawned upload has finished
    // updating `state->in_flight`. Required on every early-exit path so the
    // detached coroutines don't outlive references captured by `spawn_upload`.
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

    // Limits the in-flight upload count. Returns false if we should wait.
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
            cmlb::core::Logger::info("mirror_url: task={} cancelled (source={})",
                                     meta.id.value(),
                                     registry_cancelled ? "registry" : "coroutine");
            co_return cmlb::core::error(cmlb::core::ErrorCode::Cancelled,
                                        "mirror: cancelled by user");
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
                    final_status.error_message.value_or(std::string{"downloader reported error"});
                co_return co_await fail_with("download", cmlb::core::ErrorCode::Aria2Rpc, msg);
            }
            if (final_status.state == download_ns::DownloadState::Removed) {
                cancel_all_uploads();
                co_await drain_uploads();
                co_return co_await fail_with(
                    "download", cmlb::core::ErrorCode::Cancelled, "download removed externally");
            }
            // Treat `Seeding` as "files are on disk, upload-ready" — qBit
            // never emits `Complete` because it transitions straight from
            // Downloading → uploading/stalledUP/forcedUP (mapped to Seeding).
            // Without this, torrent mirrors would never exit the poll loop.
            if (final_status.state == download_ns::DownloadState::Complete
                || final_status.state == download_ns::DownloadState::Seeding) {
                download_complete = true;
            }

            // Pipeline newly-complete files into the upload pool.
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

            // Render combined progress through the throttled renderer.
            std::array<download_ns::DownloadStatus, 1> active{final_status};
            (void)co_await progress_renderer_.render(
                request.chat, std::span<const download_ns::DownloadStatus>{active});
        }

        // ---- Termination check -----------------------------------------
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
                if (state->first_error)
                    upload_err = *state->first_error;
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

    // ---- Fallback: non-pipelined upload when no files were processed ----
    upload_ns::UploadResult upload_outcome{};
    bool any_uploaded;
    {
        std::lock_guard lk{state->mu};
        any_uploaded = state->completed > 0;
        if (any_uploaded && !state->results.empty()) {
            upload_outcome = state->results.front();
        }
    }

    if (!any_uploaded) {
        // Either pipelining was disabled (backend opted out via
        // supports_pipelining()) or the downloader reported no `files`
        // entries during the loop. Fall back to the original one-shot
        // upload path using `final_status`.
        if (auto t = task.begin_upload(); !t) {
            co_return co_await fail_task("state", t.error());
        }
        co_await persist_best_effort("begin_upload_fallback");
        (void)co_await messenger_.edit_html(request.chat,
                                            *status_msg,
                                            fmt::format("<b>Uploading</b> via <code>{}</code>",
                                                        cmlb::core::escape_html(uploader.name())));

        if (final_status.files.empty()) {
            co_return co_await fail_with("download",
                                         cmlb::core::ErrorCode::Internal,
                                         "downloader reported no produced files");
        }
        if (final_status.save_path) {
            auto dir_res = co_await uploader.upload_directory(*final_status.save_path, up_cfg, {});
            if (!dir_res) {
                co_return co_await fail_task("upload", dir_res.error());
            }
            if (!dir_res->empty()) {
                upload_outcome = dir_res->front();
            }
        } else {
            auto file_res = co_await uploader.upload_file(final_status.files.front(), up_cfg, {});
            if (!file_res) {
                co_return co_await fail_task("upload", file_res.error());
            }
            upload_outcome = *file_res;
        }
    }

    // ---- Completed -----------------------------------------------------
    if (!transitioned_to_uploading) {
        // begin_upload() was never called along the pipelined path (e.g.
        // the fallback path also missed it). Drive the state machine here.
        if (auto t = task.begin_upload(); !t) {
            // Already past Uploading? best-effort persist whatever we have.
        }
    }
    if (auto t = task.mark_completed(); !t) {
        co_return co_await fail_task("state", t.error());
    }
    co_await persist_best_effort("mark_completed");
    // Release the downloader-side resources. For torrents in Seeding this
    // stops the seeding session; for HTTP/aria2 the entry is removed from
    // the active list. `delete_files=false` so the on-disk artefacts stay
    // available if the operator hasn't enabled auto-cleanup elsewhere.
    (void)co_await downloader.remove(gid, false);
    {
        // Upload-outcome strings may carry user-controlled bytes (filename
        // pieces in `file_id`, third-party URLs in `link`). Escape and
        // bound them before composing the success HTML.
        const std::string& outcome =
            upload_outcome.link.empty() ? upload_outcome.file_id : upload_outcome.link;
        (void)co_await messenger_.edit_html(
            request.chat,
            *status_msg,
            fmt::format("<b>Completed</b>: <code>{}</code>",
                        cmlb::core::escape_html(cmlb::core::truncate_for_display(outcome, 400))));
    }
    cmlb::core::Logger::info("mirror_url: task={} completed", meta.id.value());
    co_return meta.id;
}

} // namespace cmlb::application
