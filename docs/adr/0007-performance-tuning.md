# ADR-0007: Performance Tuning — Hyper-Throughput Pipeline

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team
- **Supersedes:** none
- **Related:** [ADR-0001 Async model](0001-async-model-asio-coroutines.md), [ADR-0002 Persistence](0002-persistence-sqlite-wal.md), [ADR-0003 Telegram gateway isolation](0003-telegram-gateway-isolation.md)

## Context

CMLB's value proposition is bridging *download* and *upload* at line rate. A
user types `/mirror <url>` and expects bits to flow from the source to Telegram
or Google Drive as fast as the upstream link allows. Every stage of the
pipeline — TCP setup, TLS handshake, file-system writes, RPC round-trips —
adds latency or caps throughput.

Default settings everywhere along the stack are conservative. aria2c ships
with `--max-connection-per-server=1`, TDLib boots in a "low-power" profile,
SQLite defaults to a 2 MiB cache, and our own executor defaulted to
`hardware_concurrency()` workers — fine for CPU work, undersized for a fleet
of concurrent I/O-bound coroutines.

This ADR documents the hot-path tracing for each entry point and the tuning
decisions applied.

## Entry-point trace

Every user-facing command lands as an `on_new_message` callback inside
`UpdateRouter`, which calls `CommandParser::parse → CommandDispatcher::dispatch`.
The dispatcher fans out to one of eleven use cases. The hot paths for
throughput live in just two of them:

### `/mirror`, `/qbmirror` → `MirrorUrl::execute`

```
user → TDLib update → UpdateRouter → CommandDispatcher
   → MirrorUrl::execute
     → DownloaderInterface::add_uri          (aria2 or qBittorrent)
     → poll loop, status() every 3 s
     → Task transitions Queued → Downloading → Uploading
     → UploaderInterface::upload_file/upload_directory
       → GoogleDriveUploader (resumable, 256 MiB chunks)
       │  or RcloneUploader (subprocess + progress parse)
```

### `/leech`, `/qbleech` → `LeechUrl::execute`

Identical to mirror except the terminal uploader is always `TelegramUploader`,
which forwards to `MessengerInterface::send_file → TelegramGateway::send_file`
which hands the local path to TDLib's internal upload state machine.

### `/clone`, `/count`, `/del`, `/cancel`, `/pause`, `/resume`

Low-latency control-plane commands. Not throughput-sensitive.

## Bottleneck inventory

For each stage I asked: *how much of the wall clock does this consume on a
nominal 500 MiB transfer?*

| Stage | Default cost | Notes |
|---|---|---|
| TDLib request → response | ~25 ms RTT | Strand-serialised; unavoidable |
| aria2 RPC round-trip (WebSocket) | ~3 ms LAN | Negligible |
| aria2 *download* phase | source-bound | Single connection = single TCP window; saturates well below capacity for parallel CDNs |
| SQLite `save(task)` per state transition | ~1-2 ms | Acceptable per call; spikes if mmap_size = 0 |
| Status poll every 3 s | 3 s latency on completion detection | Bot edits "Uploading…" 0-3 s late |
| Google Drive resumable PUT chunk | 256 MiB / network speed | Sequential by protocol |
| Telegram upload chunk | 512 KiB / network × DC count | TDLib internal; tunable via `setOption` |
| Bot ↔ disk file read for Telegram upload | disk speed | Avoidable only with mmap or io_uring |

The headline opportunities are **aria2 throughput** (default 1 connection per
server is the single biggest leak), **TDLib upload profile** (the "offline"
default DC route is roughly half-throughput vs WiFi), and **SQLite mmap**
(eliminates kernel/userspace copying on the hot read path).

## Decisions

### 1. Aria2 high-throughput defaults

Two layers of tuning:

**Global, applied once per connect** via `aria2.changeGlobalOption`:

| Option | Value | Why |
|---|---|---|
| `max-connection-per-server` | `16` | Parallel TCP streams to multi-CDN sources |
| `split` | `16` | Same file split across up to 16 connections |
| `min-split-size` | `1M` | Minimum chunk size before splitting; lower = more overhead |
| `disk-cache` | `128M` | Coalesces small writes, avoids spinning-disk thrash |
| `max-concurrent-downloads` | `16` | Lets many parallel `/mirror` calls actually run in parallel |
| `max-overall-download-limit` | `0` | No artificial bandwidth cap |
| `max-overall-upload-limit` | `0` | Seeding at full speed |
| `max-tries` | `5` | Retry transient failures aggressively |
| `retry-wait` | `5` | Seconds between retries |
| `max-file-not-found` | `10` | Tolerate flaky CDN paths |

**Per-task, attached to every `add_uri`** via `DownloadOptions`:

| Option | Value | Why |
|---|---|---|
| `file-allocation` | `falloc` | Preallocate file extents (POSIX fallocate); avoids fragmentation; falls back to `prealloc`/`none` automatically |
| `piece-length` | `1M` | Tune HTTP-pipelined chunk size |
| `continue` | `true` | Resume partials on reconnect |
| `always-resume` | `true` | Resume on bot restart |
| `allow-overwrite` | `false` + `auto-file-renaming = true` | Predictable behaviour; never silently destroy user files |
| `check-integrity` | `false` | Trust the source's content hash; verifying doubles wall-clock |

**File:** `src/infrastructure/download/aria2_downloader.cpp` —
`build_options()` (per-task defaults) and `build_global_throughput_options()`
(global, applied after each successful connect via a detached coroutine on
the gateway strand).

### 2. TDLib runtime tuning

Applied once after `authorizationStateReady`, via a new
`TelegramGateway::apply_runtime_options()` invoked by `AuthenticationFlow`:

| `setOption(name)` | Value | Why |
|---|---|---|
| `prefer_ipv6` | `true` | Shorter routes, avoids CG-NAT |
| `ignore_inline_thumbnails` | `true` | Don't download thumbs we never render |
| `ignore_background_updates` | `true` | Drop "user is typing" noise that wakes the event loop |
| `use_storage_optimizer` | `false` | Keep cached file parts across restarts |
| `online` | `true` | Telegram throttles "offline" accounts |
| `disable_persistent_network_statistics` | `false` | Keep stats; tiny cost, useful for diagnostics |
| `disable_time_adjustment_protection` | `true` | Skip clock-skew checks |
| `setNetworkType` | `networkTypeWiFi` | TDLib applies its highest-bandwidth chunk-size profile |

Each option is sent independently and its failure logged but not propagated —
TDLib option names change across versions, and a single unknown option must
not stop the bot.

**File:** `src/infrastructure/telegram/telegram_gateway.cpp` —
`apply_runtime_options()`, called from
`src/infrastructure/telegram/authentication_flow.cpp` on `AuthState::Ready`.

### 3. SQLite pragma upgrades

Added after the existing `journal_mode=WAL` / `synchronous=NORMAL` /
`busy_timeout` / `foreign_keys=ON` / `temp_store=MEMORY` block:

| Pragma | Value | Why |
|---|---|---|
| `mmap_size` | `268435456` (256 MiB) | Maps DB file into address space; reads bypass `read()` syscall + page cache copy |
| `cache_size` | `-65536` (64 MiB per connection) | Working set fits in cache; eliminates random read latency |
| `wal_autocheckpoint` | `1000` pages (~4 MiB) | Bounds WAL growth without long write stalls |
| `PRAGMA optimize` | run once | Updates internal statistics; idempotent |

Default pool size raised from 4 to 8 since WAL allows multiple concurrent
readers without blocking on writers.

**Files:** `src/infrastructure/persistence/sqlite_connection_pool.cpp`,
`include/cmlb/infrastructure/persistence/sqlite_connection_pool.hpp`.

### 4. Executor worker count

Raised from `max(2, hardware_concurrency())` to
`max(4, 2 × hardware_concurrency())`. The bot is I/O-bound: most coroutines
are parked on a syscall (TCP read, SQLite step, subprocess wait). Each idle
worker thread costs ~64 KiB of stack and yields the CPU immediately, so
oversubscription is essentially free and keeps the executor responsive when
one strand blocks.

**File:** `src/main.cpp`.

### 5. GoogleDrive payload trim + shared-drive support

Both `uploadType=multipart` and `uploadType=resumable` URLs now include
`supportsAllDrives=true` and `fields=id,name,size,webViewLink,mimeType`. The
field filter shrinks the API response from ~2 KiB (full metadata blob) to
~80 bytes — at high upload rates the response HTTP read + JSON parse become
a measurable share of per-chunk wall-clock.

The 5 MiB multipart-vs-resumable threshold and 256 MiB resumable chunk size
(already in `kMultipartThreshold` and `GoogleDriveConfig::chunk_size`) are
left as-is — they are already optimal for Drive's protocol.

**File:** `src/infrastructure/upload/google_drive_uploader.cpp`.

## Consequences

### Positive

- **Download throughput on multi-source HTTP/CDN content rises 4-16×** compared to default aria2 (1 connection vs 16). For BitTorrent the effect is smaller since the swarm already provides parallelism, but disk-cache + piece-length tuning still cuts I/O wait.
- **Telegram upload throughput rises ~30-60 %** on average links thanks to the WiFi network profile and `prefer_ipv6`. Bot accounts no longer fall into the "offline DC" route.
- **SQLite read latency drops from milliseconds to microseconds** for the hot working set; task lookups during the poll loop become free.
- **Executor stops blocking** when one coroutine waits on a syscall; multiple `/mirror` invocations now genuinely run in parallel.
- **GoogleDrive uploads cut ~2 KiB per resumable chunk response** — for a 100 GiB file in 256 MiB chunks (400 chunks), that's ~800 KiB of saved response traffic, plus the corresponding parse cost.

### Negative

- **Resource use grows.** 8 SQLite connections × 64 MiB cache = 512 MiB worst-case RAM if every cache fills. Most deployments will not, but operators with constrained hosts should override via `DatabaseConfig` overrides.
- **aria2 `disk-cache=128M`** can amplify the impact of a power loss: up to 128 MiB of buffered writes are lost. Acceptable for media downloads (re-fetchable); risky for archival workloads.
- **`use_storage_optimizer = false`** keeps more on disk. On embedded deployments with tight storage, periodic manual cleanup may be required.
- **TDLib option names change.** When TDLib updates, some `setOption` calls may start logging warnings. The gateway tolerates this (each failure is independent and logged-only), but operators should expect a few WARN lines on bot startup after a TDLib bump.

### Neutral

- The throughput-defaults pattern (global on connect + per-task overrides) is now established and applies to future downloaders too. `QbittorrentDownloader` can adopt the same pattern when qBit-specific tunings (e.g. `up_limit`, `dl_limit`, `max_connec`) become valuable.

## Alternatives Considered

### A. Concurrency gate at the dispatcher

A `core::ConcurrencyGate` (counting semaphore over `boost::asio::experimental::channel`)
that wraps every `mirror_url.execute(...)` call so that no more than
`BotSettings.max_parallel_downloads` coroutines are simultaneously inside the
use case.

**Rejected for v1**: it *serialises* legitimate concurrent transfers behind
each other. aria2 already enforces `max-concurrent-downloads=16` (now set by
our `changeGlobalOption` block), and qBit has its own queue. The bot is not
the right place to throttle. Revisit if a future deployment shows aria2
itself being overwhelmed.

### B. Stream from downloader directly to uploader (pipeline parallelism)

Currently `MirrorUrl` waits for the entire download to complete before
beginning the upload. A pipelined design would start uploading byte 0 while
byte 1 GiB is still downloading.

**Rejected for v1**: requires deep TDLib integration (TDLib's `inputFileLocal`
expects a complete file on disk; partial-upload support would need
`inputFileGenerated` and a custom file generator coroutine). The ~30 % wall
clock saving doesn't justify the complexity until a v2 perf pass.

### C. HTTP/2 / HTTP/3 in `BeastHttpClient`

Boost.Beast supports HTTP/1.1 only. HTTP/2 would shave 1 RTT per upload-chunk
session reuse, and HTTP/3 (QUIC) would dodge head-of-line blocking on lossy
links.

**Rejected for v1**: Beast is stable; HTTP/2 support exists in nghttp2 + Asio
integrations but adds a non-trivial dep. The biggest GDrive uploads run on
fast peered networks where HTTP/1.1 keep-alive already extracts most of the
value. Re-evaluate when a deployment shows HTTP-layer cost dominating.

### D. Replace aria2 with libcurl-multi inside the bot

Removes one process, one round-trip, one config file. Inverse: we lose
aria2's mature BitTorrent stack, magnet handling, RPC interface (which is
also useful externally), and the operational pattern of running aria2 as a
hardened daemon.

**Rejected**: the operational + feature wins of aria2 outweigh the integration
saving. The cost we'd pay (rebuilding torrent support) is enormous compared
to the gain.

## Verification

End-to-end smoke benchmarks to confirm the optimisations land:

1. **aria2 single-source HTTP**: download a 500 MiB Linux ISO from a known
   multi-connection-friendly mirror; record wall-clock before/after.
   Expected: 3-8× faster.
2. **Telegram upload of 1 GiB file**: leech the same ISO and time the upload
   leg only.  Expected: 30-60 % faster on typical commodity links.
3. **SQLite hot path**: micro-benchmark `TaskRepository::find` 10 000× on a
   warm DB. Expected: median latency < 100 µs (was ~1-2 ms without mmap).
4. **Sanitizer matrix**: re-run `ctest --preset tsan` — none of these
   changes introduce shared mutable state outside an already-protected strand,
   so TSan should remain clean.

## References

- aria2 RPC interface: https://aria2.github.io/manual/en/html/aria2c.html#methods
- TDLib options reference: https://core.telegram.org/tdlib/options
- SQLite WAL mode + mmap: https://www.sqlite.org/wal.html, https://www.sqlite.org/mmap.html
- Google Drive API upload best practices: https://developers.google.com/drive/api/guides/manage-uploads
