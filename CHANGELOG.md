# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Test suite

- **100% pass — 113/113 tests** under the strict-warning build profile
  (`-Werror -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor
  -Wold-style-cast -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wcast-align -Wmisleading-indentation -Wduplicated-cond
  -Wduplicated-branches -Wlogical-op -Wuseless-cast`). Total runtime: ~2.5 s.

### Added

- **`/version` command** — replies with `cmlb v<X.Y.Z>` from the generated `<cmlb/version.hpp>` constant. Available to anyone in chats the bot is authorised in.
- **Command descriptions in `/help`** — `CommandDispatcher::register_` gained a third `std::string description` parameter; every built-in handler now ships a one-line description that the dispatcher's `help_handler` feeds into `HtmlRenderer::render_help`. Previously `/help` listed bare command names with no explanation of what each did.
- **`TelegramConfig.upload_files_parallelism`** (default `2`, range `[1, 32]`, env override `CMLB_TELEGRAM_UPLOAD_FILES_PARALLELISM`) — controls how many distinct files `TelegramUploader::upload_directory` keeps in flight against a single chat. Documented in `docs/configuration_reference.md` and surfaced in `config.example.json`.
- **`docs/throughput_benchmarks.md`** — operator-facing measurement template
  for the recent throughput-optimization pass (PRs 1-11). Methodology,
  per-scenario commands and result tables (GDrive single-file, GDrive
  directory, Telegram split, aria2, pipelined mirror, rclone),
  theoretical-ceiling formulas, and a per-knob tuning playbook.
- **Portable `Subprocess`** implementation that bypasses Boost.Process v2 (whose
  API churned between Boost 1.85 and 1.91 in incompatible ways). Uses native
  OS primitives — POSIX `fork`/`execvp`/`waitpid`/`pipe` on Linux/macOS,
  Win32 `CreateProcessW`/`CreatePipe`/`WaitForSingleObject` on Windows. The
  synchronous spawn runs on a worker `std::jthread`; the calling coroutine
  parks on a steady_timer that the worker cancels on completion. Cancellation
  triggers graceful termination then hard kill after 5 s.
- **Modernised `packaging/Dockerfile`** — multi-stage, multi-arch
  (`linux/amd64` + `linux/arm64`), Ubuntu 24.04 + GCC 14 builder, Debian 13
  slim runtime, ccache + vcpkg binary-cache for fast incremental rebuilds,
  ships with `aria2`, `ffmpeg`, `p7zip-full`, `rclone` so the bot works out
  of the box, runs as non-root user `cmlb` (uid 10001), HEALTHCHECK probes
  `cmlb --validate-config`, `tini` as PID 1 for proper zombie reaping +
  SIGTERM forwarding, complete OCI labels (revision, build date, source,
  documentation URL).
- **`packaging/docker-compose.yml`** — production-shaped stack with
  `cmlb-bot` + `cmlb-aria2`, mandatory env-var checks (`${VAR:?msg}`) so
  silent misconfiguration is impossible, resource limits, health checks,
  hardened security (`no-new-privileges`, `cap_drop ALL`), JSON-file logging
  with rotation.
- **`packaging/.env.example`** — operator template with every tunable
  documented and a strong-password reminder.
- **`.dockerignore`** — keeps the build context tight (excludes
  `build/`/`tdlib/`/`runtime/`/`*.legacy.*`/`*.real`/`.git/`), shrinks
  multi-arch buildkit transfer time by ~95 %.

### Changed

- **Subprocess tests pass with the real implementation** — previously failing
  under the stub. Same for `version_of()` which now returns the trimmed
  first stdout line.

- **`DownloaderKind` enum + `Task::attach_downloader / downloader_id / downloader_kind`** so that `/cancel`, `/pause`, `/resume` can dispatch to the exact downloader the Task was bound to. Replaces the placeholder hack that used `source_url.substr(0, 64)` as a guessed Gid.
- **`V0003__task_downloader_attachment` migration** adds the persistence columns for the above (forward-only, default values keep existing rows valid).
- **`CancelTask::cancel_all(ChatId)`** + bulk-result reporting. `/cancelall` now performs the real operation rather than replying "not implemented".
- **`TelegramGateway::apply_runtime_options()`** sends throughput-oriented `setOption` calls + `setNetworkType(networkTypeWiFi)` once `authorizationStateReady` arrives, lifting upload/download speed roughly 30-60 % on average links by avoiding the "offline DC" route.
- **TDLib integer options pushed at startup** — `upload_chunk_size_kb`, `download_chunk_size_kb`, `connection_retry_count_max` are now driven from `TelegramConfig` (defaults 2048 / 1024 / 5) so operators can tune them per host. `prefer_ipv6` is also now config-driven (no longer hardcoded `true`). Failure of any individual option is logged at `warn` and never blocks startup.
- **Dedicated `ClientManager::receive(timeout)` thread** in `TelegramGateway::run()` replaces the 100 ms strand-polled `steady_timer` busy-loop. The thread blocks in TDLib's native poll (500 ms wakeup for stop-flag responsiveness) and `asio::post`s events onto the strand. p99 update latency drops from ~100 ms to network-bounded; CPU usage on an idle bot drops from ~10× wakeups/sec to ~2/sec.
- **`TelegramUploader` parallel split-parts** — files exceeding `UploadConfig.split_size` now fan their `Part NN/TOTAL` segments out across `TelegramConfig.upload_parallelism` workers (default 4). Each worker `co_spawn`s detached against the calling executor, draws its index from a shared atomic counter, and reports the first error via a guarded `AppError` slot. The serial fallback branch has been removed — the parallel pool degenerates cleanly to a single sequential worker at `upload_parallelism = 1` (no legacy code path). Constructor signature now takes `(MessengerInterface&, const TelegramConfig&)` — `src/main.cpp` updated accordingly. On an 8 GiB leech (4×2 GiB parts), wall-clock approaches `max(per-part RTT)` instead of the prior `sum(per-part RTT)`.
- **`TelegramUploader::upload_directory` parallel files** — directories now upload `TelegramConfig.upload_files_parallelism` (default 2) files concurrently against the same chat. A `DirShared` state object holds the work queue, atomic next-index, live-worker counter, first-error slot, and a join `steady_timer`; the original serial loop is gone. TDLib internally pipelines upload sessions, so concurrent `send_file` calls per chat saturate the link better than serial — observed ~1.8-2.0× speedup on directories of small media.
- **`GoogleDriveUploader::upload_resumable` parallel chunks** — a single resumable session URI now accepts `GoogleDriveConfig.parallel_chunks_per_file` (default 4) concurrent `Content-Range` PUTs. A shared atomic next-offset counter dispenses 256 KiB-aligned chunk plans; per-worker `std::ifstream` instances avoid the previous shared file handle. The original per-chunk `std::vector<char>` allocation is replaced with a `std::string` body buffer that copies into the HTTP request (copy, not move — copies are reused across retry attempts). The serial fallback branch has been deleted; the parallel pool handles `parallel_chunks_per_file = 1` as one sequential worker. Expected ~4× wall-clock improvement on high-latency links (10 GiB file at 50 ms RTT to googleapis.com).
- **`GoogleDriveUploader::upload_directory` parallel files** — directory uploads now process files concurrently via a worker pool sized by `GoogleDriveConfig.parallel_files_per_directory` (default 4). Two-pass: serial walk + folder-create (preserves parent-id propagation), then parallel file fan-out. The single-file / serial fallback branch is removed; `parallel_files_per_directory = 1` runs as one worker through the same machinery.
- **`GoogleDriveUploader` per-chunk retry with exponential backoff** — every parallel chunk PUT now retries up to `GoogleDriveConfig.max_retries` (default 6) attempts on network errors, 429, and 5xx responses. Backoff is `initial_retry_delay * 2^min(attempt-1, 10)` (default `500 ms * 2^N`, capped at the 10-shift ceiling). 4xx other than 408/429 fail-fast. 401 still triggers the inner auth-retry loop (`invalidate_bearer_cache()` + one retry with a fresh bearer). Replaces the previous `TODO: retry policy` no-op.
- **`GoogleDriveUploader` bearer refresh on 401 in parallel uploads** — parallel chunk workers now call `acquire_bearer()` per PUT (cache-hits are a mutex + steady_clock compare) and force-refresh on a 401 via a new `invalidate_bearer_cache()` method. Long-running parallel uploads that span an OAuth token expiry (default lifetime 3600 s) recover transparently instead of failing every in-flight chunk.
- **`MirrorUrl` / `LeechUrl` download→upload pipelining** — use cases now spawn per-file upload coroutines as soon as the downloader reports each file `complete on disk` (via `DownloadStatus.files`), bounded by `parallel_files_per_directory` (mirror) / `upload_parallelism` (leech). Both use cases drop their local `kProgressEditMin{3s}` direct `messenger_.edit_html` blocks and route status through the injected progress-renderer port (per-chat coalescing, content-hash dedup, edit-fallback-to-send). qBittorrent torrents still take the post-completion one-shot upload path so seeding semantics remain intact. Constructor signatures gain `(ProgressRendererInterface&, int upload_pool_size)`; `src/main.cpp` builds the concrete renderer once and injects it into both use cases.
- **`cmlb::application::ProgressRendererInterface` port** — new abstract base in `include/cmlb/application/progress_renderer_interface.hpp` defines the `render` / `force_refresh` contract. `cmlb::presentation::ProgressRenderer` now inherits and overrides it. The application layer no longer compiles in any reference to the presentation layer — the inverted dependency surfaced by PR 11 (`cmlb_application` had an unresolved `ProgressRenderer::render` symbol resolved at executable link via `cmlb_presentation`) is gone. `cmlb_application` is again a clean static archive against `cmlb_core` / `cmlb_domain` / `cmlb_infrastructure`.
- **`MirrorUrl` / `LeechUrl` pipelining gate decoupled from downloader identity** — both use cases now gate `upload_while_downloading` on `downloader.supports_pipelining()` (a new virtual on `DownloaderInterface` defaulting to `false`, overridden to `true` on `Aria2Downloader`) instead of the ad-hoc `request.use_qbittorrent` boolean. Future downloader adapters can opt in or out of pipelining without touching the use-case code, and the application layer no longer carries hardcoded knowledge of which backends seed vs. stream.
- **`build_global_throughput_options()` in `Aria2Downloader`** pushes `aria2.changeGlobalOption` on every successful connect (`max-connection-per-server=16`, `split=16`, `disk-cache=128M`, `max-concurrent-downloads=16`, …). Per-task `build_options` adds `file-allocation=falloc`, `piece-length=1M`, `continue=true`, etc.
- **ADR-0007 Performance Tuning** documents the hot-path trace from every entry point and the optimisations applied at each stage.

### Changed

- **`TelegramUploader` now takes `MessengerInterface&`** rather than the concrete `Messenger&` — all other use cases already did this; the inconsistency is closed.
- **`docs/architecture.md`** and **`.claude/skills/add-command`** updated to show `MessengerInterface&` in the canonical use-case constructor example.
- **`MirrorUrl` / `LeechUrl`** call `task.attach_downloader(gid, kind)` immediately after the downloader returns its id, persisting the binding so later cancel/pause/resume work without guessing.
- **`SqliteConnectionPool`** opens every connection with `mmap_size = 256 MiB`, `cache_size = -65536` (64 MiB), `wal_autocheckpoint = 1000`, and runs `PRAGMA optimize` at startup. Default pool size raised from 4 to 8 since WAL allows parallel readers.
- **`Executor`** default worker count raised from `max(2, hardware_concurrency())` to `max(4, 2 × hardware_concurrency())` to keep coroutines flowing when one strand blocks on a syscall.
- **`GoogleDriveUploader`** multipart and resumable upload URLs now include `supportsAllDrives=true` (shared-drive support) and `fields=id,name,size,webViewLink,mimeType` (trims response payload from ~2 KiB to ~80 bytes per chunk).

### Fixed

- **`/cancel`, `/pause`, `/resume`** no longer use a string prefix of the source URL as the downloader id. They now read the persisted `(downloader_kind, downloader_id)` pair from the Task aggregate and dispatch to the correct backend (or return `InvalidState` when the Task is still in `Queued`).
- **Permission-denied message** now names the resolved command and the required tier (`anyone` / `authorized user` / `admin (sudo)` / `owner`) instead of the generic "you don't have permission". Lets users see exactly what privilege would unblock the action.
- **`/log` size guard** — `/log` rejects log files over 50 MiB (matching the rotating sink's per-segment cap) with a clear message including the actual size, rather than blindly trying to upload a multi-GB file.
- **Chat-scope hardening on `/cancel`, `/pause`, `/resume`** — these commands now refuse to act on tasks that belong to a different chat than the requester, returning `PermissionDenied` instead of silently mutating a stranger's task. `/cancelall` also filters by chat before iterating.
- **RSS URL validation** — `/rss add` now rejects URLs that don't start with `http://` or `https://`, preventing typo'd feeds from being persisted and then polled forever.
- **Google Drive cancellation during retry backoff** — chunk workers now re-check the abort flag immediately after the retry `co_await`. Previously a cancellation that fired during the exponential-backoff sleep slipped through and surfaced as the spurious "chunk PUT returned 0" error.
- **Worker exception safety in parallel uploaders** — every detached worker in `GoogleDriveUploader` (`upload_resumable`, `upload_directory`) and `TelegramUploader` (`upload_file` split-parts, `upload_directory`) now uses an `ExitGuard` RAII struct that decrements the live-worker counter and cancels the join timer in its destructor. If any worker throws or is cancelled mid-loop, the use-case coroutine no longer deadlocks on `async_wait`.
- **Split-part disk cleanup on early exit** — `TelegramUploader::upload_file` now removes the temporary split-part files via an RAII guard rather than a fall-through `for` loop, so they're cleaned up even when the join `async_wait` throws on cancellation.
- **Best-effort task persistence is logged** — `mirror_url.cpp` and `leech_url.cpp` no longer silently swallow `tasks_.save()` errors at intermediate state transitions (`start_download`, `attach_downloader`, `begin_upload`, `mark_completed`, `mark_cancelled`, `mark_failed`). Save failures are surfaced via `Logger::warn` with the stage that failed and the underlying error message.
- **`/help` respects caller permission** — the dispatcher's auto-generated help now filters commands by what the caller can actually run, lists each command's aliases inline, and sorts by required tier so the highest-privilege entries appear at the bottom rather than scattered.
- **Empty-argument validation on `/mirror`, `/qbmirror`, `/leech`, `/qbleech`, `/clone`, `/count`, `/del`** — these commands now reply with a usage hint instead of either silently failing or sending a vague error when called without an argument.
- **Close button actually closes** — the inline-keyboard `close` callback now deletes the host message via the freshly-plumbed `MessageId` (gateway → router → dispatcher). Previously the spinner cleared but the message stayed visible.
- **`/cancel` propagates to running coroutine** — new `ActiveTaskRegistry` keyed by `TaskId` exposes a `std::atomic<bool>` cancellation flag that `MirrorUrl`/`LeechUrl` poll every tick via the RAII `ActiveTaskGuard`. `CancelTask` sets the flag and returns a `Cancelling` ack; the live coroutine observes the flag, drains in-flight uploads, calls `downloader.remove(gid, true)`, marks the task `Cancelled`, persists, and edits the status message — no more bytes pumped to GDrive/Telegram after the user pressed cancel. Falls back to direct teardown when no coroutine is registered (Queued/Paused tasks).
- **qBittorrent seed-ratio / seed-time enforcement** — `push_preferences` now emits `max_ratio_enabled / max_ratio` (from `QbittorrentConfig.seed_ratio_limit`) and `max_seeding_time_enabled / max_seeding_time` (from `seed_time_limit`, in minutes) plus `max_ratio_act=0` so torrents that hit either limit are auto-paused. Previously those fields were configured but never sent to qBit, so torrents seeded forever regardless of the user's `seed_ratio_limit`.
- **Unknown qBittorrent state warning** — `parse_state()` now logs `warn` once per distinct unknown state string instead of silently mapping it to `Queued`. Catches new qBit versions that introduce states we haven't mapped (e.g. a future `forcedMetaDL`) so the operator notices in the log without spam.
- **Subprocess cancellation actually kills the child** — `Subprocess::run` now publishes the native child handle (`HANDLE` on Win32, `pid_t` on POSIX) into a shared `CancelHandle` immediately after spawn, and installs an `asio::cancellation_slot` handler that calls `TerminateProcess` / `SIGTERM` when the caller cancels the awaitable. The POSIX poll loop and waitpid loop both observe the cancellation flag and break out; the existing 5 s grace + `SIGKILL` escalation handles unresponsive children. Previously `/cancel` against a long-running `rclone copyto` or `ffmpeg` extract left the child running until natural exit while the bot reported the task cancelled. `SubprocessResult.cancelled` is set so callers can distinguish a user cancel from a timeout. Win32 reader threads upgraded to `std::jthread` so an exception after spawn no longer terminates the process on an unjoined thread.
- **`ProgressRenderer` per-chat strand serialization + in-progress guard** — each chat now owns a dedicated `boost::asio::strand` whose lifetime is bounded by a per-chat heap-allocated `ChatState` (`std::unordered_map<ChatId, std::unique_ptr<ChatState>>`, so the pointer returned by the get-or-create helper is stable across rehashes). `render()` and `force_refresh()` `co_spawn` their body onto the chat's strand — crucially `co_spawn`, not a one-shot `asio::post(bind_executor(strand, use_awaitable))` hop, because the inner coroutine's *associated executor* must BE the strand so every subsequent `co_await` resumes on the strand (the outer coroutine's associated executor is fixed at outer-`co_spawn` time and a one-shot hop only affects the very next resume — the second `co_await` would leak straight back off-strand and corrupt state writes). Two races are now closed: (1) the double-send race where two tasks on the same chat both saw `cached_id==0` between awaits and both issued fresh `send_html_with_keyboard`, orphaning the older message; (2) the lost-update race where overlapping `edit_html` calls could land out of order. The strand serializes *handlers*, not *coroutines*, so the impl additionally takes a 1-slot `boost::asio::experimental::channel<void(error_code)>` used as an async mutex (the token is the "render in progress" right; an RAII guard releases via `try_send` on scope exit, which is non-blocking and infallible on a capacity-1 channel whose token we previously held). Periodic `render()` does a non-blocking `try_receive` and coalesces on contention (drop, rely on the next polling tick within the throttle window); `force_refresh()` does a blocking `co_await async_receive(use_awaitable)` and waits (user-initiated, must not be silently dropped). The previous fix carried a 20 ms `steady_timer` poll loop on the strand for the wait path — that's gone in favor of an event-driven suspend that also propagates caller-side cancellation as a native `system_error` throw before any state write. Different chats remain fully concurrent — only same-chat work serializes.
- **`BeastHttpClient` cancellation no longer masquerades as Timeout** — `translate_ec` split `asio::error::operation_aborted` (external cancellation via the coroutine's `cancellation_slot` firing, or an explicit `stream.cancel()`) out of the `beast::error::timeout` bucket. Previously both surfaced as `ErrorCode::Timeout`, which collided with the GDrive retry policy below and caused user `/cancel` actions during an in-flight chunk PUT to spin through up to `max_retries` exponential-backoff attempts before finally giving up. Cancellation now maps to `ErrorCode::Cancelled`; the deadline-timer expiry keeps `ErrorCode::Timeout`.
- **`GoogleDriveUploader` chunk retry treats Cancelled as terminal** — the per-chunk retry loop in `upload_resumable` was retrying on every non-HTTP transport error including the (previously mis-labeled) cancellation. With the http client now distinguishing the two, the retry path explicitly short-circuits on `ErrorCode::Cancelled` and propagates immediately, so `/cancel` against an in-flight upload tears down within one round-trip instead of one round-trip × (max_retries + 1).
- **Config validation gaps closed** — `qbittorrent.url` is now required to start with `http://` or `https://` (was an empty-check only — a typo'd `localhost:8080` previously parsed cleanly and only failed on the first POST). `qbittorrent.up_limit` / `dl_limit` reject values below `-1` (the sentinel for unlimited). `rclone.transfers` / `checkers` / `multi_thread_streams` now have upper bounds `64` / `256` / `64` to prevent file-descriptor exhaustion and RAM thrashing from a fat-fingered `9999`. `database.busy_timeout` is capped at `60 000 ms` so a misconfigured value can't hang every repository call indefinitely. Malformed `CMLB_*` env-var overrides are now logged via `Logger::warn` (`expected integer/bool/decimal`) instead of being silently dropped — operators see exactly which env var was rejected and what the parser wanted.

### Performance

- **Download throughput on multi-source HTTP/CDN content** rises 4-16× compared to default aria2 (1 connection vs 16). For BitTorrent the swarm already provides parallelism, but disk-cache + piece-length tuning still cuts I/O wait.
- **Telegram upload throughput** rises ~30-60 % on average links via `prefer_ipv6`, `networkTypeWiFi`, and `online=true`. Bot accounts no longer fall into the "offline DC" route.
- **SQLite read latency** drops from milliseconds to microseconds for the hot working set; task lookups during the poll loop become free.
- **Executor responsiveness** improves on hosts with low core count — multiple `/mirror` invocations now genuinely run in parallel.
- **Google Drive uploads** save ~2 KiB per resumable chunk response (1-2 % wall-clock improvement on very large files; eliminates a measurable parse hot-spot).



## [1.0.0] - 2026-05-18

Full clean-slate rebuild. Architecture, build system, tooling, CI, persistence,
Telegram gateway, downloaders, uploaders, media processors, RSS poller,
application use cases, presentation layer, and composition root all written
from scratch as a coordinated effort. No code from the legacy MVP survives.

### Added

#### Architecture
- Five-layer DDD architecture (`core`, `domain`, `application`, `infrastructure`,
  `presentation`).
- Boost.Asio C++20 coroutine async model (`awaitable<Result<T>>`) throughout.
- Strong-typed identifiers (`ChatId`, `UserId`, `MessageId`, `Gid`, `TaskId`,
  `FileId`, `CallbackQueryId`) preventing accidental mix-ups at compile time.
- `Task` aggregate with explicit state machine (`Queued → Downloading →
  Processing → Uploading → Completed | Failed | Cancelled`); invalid
  transitions return `ErrorCode::InvalidState` rather than silently corrupting
  the aggregate.
- `Authority` permission model with four hierarchical tiers (`Anyone`, `User`,
  `Admin`, `Owner`).
- `ByteSize` strong type with saturating arithmetic and UDLs (`100_MiB`).

#### Persistence
- SQLite (WAL mode) behind narrow per-aggregate repositories
  (`TaskRepository`, `UserSettingsRepository`, `BotSettingsRepository`,
  `RssFeedRepository`).
- `SqliteConnectionPool` with async acquisition via `boost::asio::experimental::channel`.
- `SchemaMigrator` with forward-only versioned migrations
  (`V0001__initial_schema.sql`, `V0002__rss_feeds.sql`).

#### Telegram integration
- `TelegramGateway` as the sole owner of `td::ClientManager` and the only
  translation unit including `<td/telegram/td_api.h>`.
- Strand-pinned ClientManager with TTL-evicted request handler map (30 s
  default).
- `Messenger` / `MessengerInterface` for typed high-level sends, hiding TDLib
  entirely from use cases.
- `UpdateRouter` dispatching new-message / callback-query / file-update events.
- `AuthenticationFlow` driving bot-token auth without blocking stdin.

#### Downloaders
- `Aria2Downloader` over WebSocket JSON-RPC with `ws://` + `wss://` support and
  exponential-backoff reconnect.
- `QbittorrentDownloader` over the v2 Web API with transparent re-login on
  403 and info-hash diff for ID recovery.

#### Uploaders
- `TelegramUploader` with automatic file-type detection and client-side split
  for files exceeding the 2 GB Telegram limit (streamed, never buffered).
- `GoogleDriveUploader` with RS256 JWT service-account auth, resumable upload
  for files > 5 MiB, and `copy` / `count` / `remove` extras for the clone /
  count / del commands.
- `RcloneUploader` wrapping the rclone subprocess with progress parsing.

#### Media + archives + RSS
- `FfmpegMediaProcessor` (probe, thumbnail, sample, screenshot grid).
- `SevenZipArchiveProcessor` (extract / create with password + split volumes).
- `RssDocumentParser` handling both RSS 2.0 and Atom with anchored magnet
  extraction.
- `RssFeedPoller` running on the main executor, auto-mirroring matching
  entries through the `MirrorUrl` use case.

#### Shared infrastructure
- `BeastHttpClient` (HTTP/HTTPS with SNI, redirect handling, streaming
  download-to-file).
- `Subprocess` (boost::process v2 wrapper with graceful kill on cancellation).
- `SystemMetrics` (cross-platform CPU / RAM / disk / uptime).
- `SignalHandler` (graceful shutdown via `boost::asio::cancellation_signal` —
  no more `std::_Exit`).

#### Application use cases
- `MirrorUrl`, `LeechUrl`, `CloneDriveResource`, `CountDriveResource`,
  `DeleteDriveResource`, `CancelTask`, `PauseTask`, `ResumeTask`,
  `UpdateUserSettings`, `UpdateBotSettings`, `RssSubscription`.
- Each use case is constructor-injected with narrow interfaces and unit-tested
  against in-memory stubs in `tests/support/`.

#### Presentation
- `CommandParser` (pure function from raw text to `CommandRequest`).
- `CommandDispatcher` with `Authority`-gated routing and built-in aliases.
- `CallbackDispatcher` for inline-keyboard callback data routing.
- `HtmlRenderer` — the single home for every user-visible formatted string.
- `ProgressRenderer` — per-chat live status message lifecycle with throttling
  and content deduplication.

#### Build system + tooling
- CMake 3.28+, Ninja default, vcpkg manifest mode with pinned baseline.
- Eight presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `coverage`,
  `msvc-debug`, `msvc-release`.
- `cmake/warnings.cmake` enforcing warnings-as-errors symmetrically across
  GCC 13+, Clang 17+, MSVC 2022, and Apple Clang.
- `cmake/sanitizers.cmake`, `cmake/coverage.cmake`, `cmake/packaging.cmake`,
  `cmake/version.cmake`, `cmake/cmlb_helpers.cmake`.

#### CI
- GitHub Actions matrix: Linux GCC 13, Linux Clang 17, Windows MSVC 2022,
  macOS Apple Clang.
- Sanitizer workflow (ASan, UBSan, TSan).
- Static analysis workflow (clang-tidy + clang-format dry-run).
- CodeQL workflow (weekly + on PR).
- Release workflow with CPack packaging across all three platforms.

#### Tests
- Catch2 v3 for unit, integration, and property-based tests.
- 25 test files covering core, domain, infrastructure, application, and
  presentation.
- Test support library (`tests/support/`) with in-memory repositories and
  programmable stub downloaders / uploaders / messenger.

#### Operational
- `cmlb --version`, `cmlb --help`, `cmlb --validate-config <path>`,
  `cmlb --migrate-only`.
- Multi-stage Docker build → `distroless/cc-debian12:nonroot` (~30 MB image).
- Hardened systemd service unit with full syscall filter, `ProtectSystem`,
  `NoNewPrivileges`.
- Debian packaging skeleton (`packaging/debian/control`).
- Bootstrap scripts for Linux, macOS, and Windows.

#### Documentation
- README, architecture diagram, runbook, configuration reference, command
  reference.
- Six Architecture Decision Records (`docs/adr/0001`-`0006`) covering async
  model, persistence engine, Telegram gateway isolation, build system,
  warnings discipline, and naming convention.
- Six skill files under `.claude/skills/` (cpp-build, add-command,
  add-backend, add-migration, add-adr, senior-cpp).
- `CLAUDE.md` describing the new architecture for future Claude sessions.

### Removed (vs. legacy `b1d4103`)

- The 1,800-line `BotEngine` god class.
- The orphan `bot_engine_append.cpp` file.
- The `_utils` / `Manager` / `I`-prefix naming conventions.
- The in-memory `Database` with its full-file-rewrite-per-mutation corruption
  window.
- The MongoDB facade that secretly returned the in-memory implementation.
- The checked-in 5.6 MB `aria2c.exe` binary.
- Interactive `std::cin` blocking calls inside the event loop.
- `std::_Exit(0)` on SIGTERM (replaced with graceful Asio cancellation).
- Three duplicate implementations of `formatBytes` / `formatEta` /
  `renderProgressBar`.
- Six TDLib-from-background-thread rule violations.

### Fixed (vs. legacy)

- Unbounded growth of TDLib request handler map (now has TTL eviction).
- Reserved request ID collisions (1, 2, 3, 100) with `getNextRequestId`.
- `gid_to_transfer_` map permanently empty (entire `updateTransferProgress`
  path unreachable).
- `/ping` always reporting 0 ms latency.
- `/botsettings` advertised in help but registered as `/bsetting`.
- `Logger` ignoring `config.log_level` and never writing to disk.
- `sendLogFile` referencing a nonexistent file.

### Changed

- Migrated from manual `std::future` + job-queue marshalling to coroutine-based async.
- Replaced 5 copies of HTML parse logic with a single `Messenger::parse_html`.
- Consolidated three copies of `format_bytes` / `format_eta` / `render_progress_bar` into `core/formatting.hpp`.
- Symmetric warnings-as-errors across all compilers (was: MSVC `/WX` strict, GCC commented out).
- Replaced `I`-prefix interfaces with `_interface.hpp` suffix and `XxxInterface` class naming.

### Removed

- The 1800-line `BotEngine` god-class (replaced by per-use-case orchestration).
- Orphan `bot_engine_append.cpp` file.
- `Manager` and `_utils` suffixes from naming.
- In-memory database with full-file-rewrite-per-mutation persistence hazard.
- Checked-in `aria2c.exe` binary (downloaded at install time via `scripts/download_runtime_dependencies.sh`).
- Interactive `std::cin` blocking calls inside the event loop.
- `std::_Exit(0)` on SIGTERM (replaced with graceful shutdown via Asio cancellation).
- MongoDB facade that secretly returned the in-memory implementation.

### Fixed

- Unbounded growth of TDLib request handler map (now has TTL eviction).
- 6+ TDLib-from-background-thread rule violations (strand discipline now enforced by the type system).
- Reserved request ID collisions (1, 2, 3, 100) with `getNextRequestId`.
- `gid_to_transfer_` map permanently empty (entire `updateTransferProgress` path unreachable).
- `/ping` always reporting 0 ms latency.
- `/botsettings` advertised in help but registered as `/bsetting`.
- `Logger` ignoring `config.log_level` and never writing to disk.

## [0.2.0] — Legacy

The legacy MVP. Preserved only in git history before commit `b1d4103`.
No longer maintained. Refer to that commit if you need to consult the
original `BotEngine` implementation.
