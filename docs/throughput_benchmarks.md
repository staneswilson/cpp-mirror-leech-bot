# Throughput benchmarks

## Purpose

This document tracks before/after throughput numbers across the recent CMLB
optimization pass (PRs 1-11). It is a measurement template: the operator
fills in the result tables once they have data from their own hardware, then
uses the tuning playbook at the bottom to converge on a working config. No
numbers are pre-filled — every cell is `_TBD_` until measured. Re-run on
each release so regressions are visible at a glance.

## Methodology

### Hardware to record

Before running any scenario, capture the host profile once:

| Field | Example | How to obtain |
|---|---|---|
| CPU model + core count | `_TBD_` | `lscpu` / `wmic cpu` / `sysctl -n machdep.cpu.brand_string` |
| RAM (GiB) | `_TBD_` | `free -g` / `wmic memorychip` / `sysctl hw.memsize` |
| Disk type (NVMe / SATA SSD / HDD) | `_TBD_` | `lsblk -d -o name,rota,model` |
| NIC + link speed | `_TBD_` | `ethtool <iface>` / `Get-NetAdapter` |
| OS + kernel | `_TBD_` | `uname -a` / `ver` |

### Network conditions

For each measurement note:

- RTT to the relevant peer / API endpoint (`ping`, `mtr`).
  - GDrive: `ping www.googleapis.com`
  - Telegram: `ping api.telegram.org` (note: real traffic goes to a DC, not
    this host)
  - Aria2 CDN: ping the actual mirror you use
- Packet loss over a 60 s window (`mtr -c 60 -r <host>`)
- Whether you are on Wi-Fi, wired, or VPN — record which

### Commands the operator runs

Build a release binary first:

```bash
cmake --preset release
cmake --build --preset release --parallel
./build/release/bin/cmlb config.json
```

Each scenario below names the bot `/command` to dispatch from the chat once
the bot is running. Edit `config.json` between runs to change the variable
under test; restart the bot if changing TDLib options that only apply at
authorization (`telegram.upload_chunk_size_kb`, `download_chunk_size_kb`,
`connection_retry_count_max`).

### Capturing timings

Two paths, in order of preference:

1. **Bot-emitted durations** — every uploader and downloader logs a
   `duration_ms` field at task completion in `logs/cmlb.log`. Filter with:

   ```bash
   tail -F logs/cmlb.log | grep -E 'task=.*(complete|finished)'
   ```

2. **External stopwatch** — record from the moment the bot acknowledges the
   `/command` in chat to the moment it sends the final "complete" message.
   Use this only if log timestamps look suspicious.

### Capturing link saturation

Run alongside the bot:

- Linux: `iftop -i <iface>` or `nload <iface>`
- Windows: Task Manager → Performance → Ethernet, or `Get-NetAdapterStatistics`
- macOS: `nettop -P` or `iftop` from Homebrew

Record peak and sustained MB/s, and the ratio of sustained to link line rate
(this is what the "saturation %" entries in tables below refer to).

### Reproducibility checklist

Before each run:

- [ ] Use the same source file (same SHA-256) across permutations
- [ ] Drop filesystem caches between runs:
  - Linux: `sync && sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'`
  - macOS: `sudo purge`
  - Windows: reboot, or use `RAMMap` → Empty Standby List
- [ ] No other heavy network traffic on the host (close browsers, syncing
      clients)
- [ ] Same `release` build profile, not `debug`
- [ ] Same TDLib session directory (avoid re-login latency skewing the first
      run); for cold-start measurements use a fresh `telegram.database_directory`
- [ ] Warm-up run discarded; report runs 2-4 averaged

## Scenarios

### Scenario 1 — GDrive single-file upload

**Workload**: 1 GiB random file (`dd if=/dev/urandom of=test1g.bin bs=1M count=1024`).

**Command in chat**: `/mirror gdrive` then upload `test1g.bin` to the bot,
or `/mirror <direct-https-url>` if hosting the file on a fast origin.

**Config knobs varied**: `google_drive.parallel_chunks_per_file`,
`google_drive.chunk_size`.

| `parallel_chunks_per_file` | `chunk_size` | wall_time | MB/s | CPU% | notes |
|---|---|---|---|---|---|
| 1 | 4 MiB | `_TBD_` | `_TBD_` | `_TBD_` | baseline, pre-PR-6 behaviour |
| 1 | 8 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 1 | 16 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 2 | 4 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 2 | 8 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 2 | 16 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 4 | 4 MiB | `_TBD_` | `_TBD_` | `_TBD_` | default |
| 4 | 8 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 4 | 16 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 8 | 4 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 8 | 8 MiB | `_TBD_` | `_TBD_` | `_TBD_` | |
| 8 | 16 MiB | `_TBD_` | `_TBD_` | `_TBD_` | watch for HTTP 416 |

### Scenario 2 — GDrive directory upload

**Workload**: 100 files × 50 MiB each in one directory.

```bash
mkdir gdrive_dir && for i in $(seq 1 100); do
  dd if=/dev/urandom of=gdrive_dir/f$i.bin bs=1M count=50 status=none
done
```

**Command in chat**: `/mirror <url-or-path-to-gdrive_dir>`.

**Config knobs varied**: `google_drive.parallel_files_per_directory` (hold
`parallel_chunks_per_file` at its default of 4).

| `parallel_files_per_directory` | wall_time | MB/s | CPU% | notes |
|---|---|---|---|---|
| 1 | `_TBD_` | `_TBD_` | `_TBD_` | sequential — pre-PR-7 |
| 2 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 4 | `_TBD_` | `_TBD_` | `_TBD_` | default |
| 8 | `_TBD_` | `_TBD_` | `_TBD_` | |

### Scenario 3 — Telegram split-file leech

**Workload**: 8 GiB file. With Telegram's 2 GiB per-message cap this is
split into 4 parts client-side.

**Command in chat**: `/leech <direct-https-url-to-8GiB-file>`.

**Config knobs varied**: `telegram.upload_parallelism`.

| `upload_parallelism` | wall_time | MB/s | CPU% | notes |
|---|---|---|---|---|
| 1 | `_TBD_` | `_TBD_` | `_TBD_` | sequential — pre-PR-8 |
| 2 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 4 | `_TBD_` | `_TBD_` | `_TBD_` | default |
| 8 | `_TBD_` | `_TBD_` | `_TBD_` | watch for FLOOD_WAIT |

### Scenario 4 — Aria2 download

**Workload**: single 1 GB file from a CDN that supports range requests
(e.g. a public Linux ISO mirror).

**Command in chat**: `/mirror <cdn-url>` (or `/leech` — same downloader path).

**Config knobs varied**: `aria2.split`, `aria2.max_connection_per_server`.
Both are capped server-side at 16.

| `split` | `max_connection_per_server` | wall_time | MB/s | CPU% | notes |
|---|---|---|---|---|---|
| 1 | 1 | `_TBD_` | `_TBD_` | `_TBD_` | baseline, single TCP stream |
| 4 | 4 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 8 | 8 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 16 | 16 | `_TBD_` | `_TBD_` | `_TBD_` | default |
| 16 | 1 | `_TBD_` | `_TBD_` | `_TBD_` | tests split independently |
| 1 | 16 | `_TBD_` | `_TBD_` | `_TBD_` | tests connections independently |

### Scenario 5 — Pipelined mirror

**Workload**: 5 GiB file from a deliberately slow upstream (a low-bandwidth
HTTP origin, or a torrent with limited peers). The point is to expose the
download/upload overlap that PR 11 enables — when both halves are
bandwidth-limited and roughly equal, the pipelined run should approach
`max(down, up)` instead of `down + up`.

**Command in chat**: `/mirror <slow-source-url>` (mirror = upload to GDrive
after download; the pipeline kicks in once the first chunk lands on disk).

**Config knobs varied**: simulate by disabling pipelining if you must — a
clean A/B compares the v1.0.0 binary (which downloads then uploads serially)
against the post-PR-11 binary. If unavailable, simply record the post-PR-11
number and the operator's measured pre-PR-11 baseline from CHANGELOG history.

| variant | wall_time_download | wall_time_upload | wall_time_total | notes |
|---|---|---|---|---|
| serial (pre-PR-11, `upload_while_downloading=false` equivalent) | `_TBD_` | `_TBD_` | `_TBD_` | total ≈ down + up |
| pipelined (post-PR-11, `upload_while_downloading=true` equivalent) | `_TBD_` | `_TBD_` | `_TBD_` | total ≈ max(down, up) |

### Scenario 6 — Rclone large directory

**Workload**: 200 files × 50 MiB each through the `rclone` uploader to a
remote backend (an S3-compatible bucket or another GDrive).

**Command in chat**: `/mirror <source>` with the upload destination resolved
to an rclone remote.

**Config knobs varied**: `rclone.transfers`, `rclone.multi_thread_streams`.

| `transfers` | `multi_thread_streams` | wall_time | MB/s | CPU% | notes |
|---|---|---|---|---|---|
| 4 | 1 | `_TBD_` | `_TBD_` | `_TBD_` | conservative — low RAM |
| 4 | 4 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 8 | 1 | `_TBD_` | `_TBD_` | `_TBD_` | default |
| 8 | 4 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 16 | 1 | `_TBD_` | `_TBD_` | `_TBD_` | |
| 16 | 4 | `_TBD_` | `_TBD_` | `_TBD_` | RAM cost ≈ 16 × `buffer_size` |

## Theoretical ceilings

Napkin-math upper bounds. Real numbers will fall short by `(1 - protocol_overhead)`
— typically 10-25 % for HTTPS, more for TLS+TCP-handshake-heavy short
transfers.

- **Single-file GDrive upload**:
  `min(link_bw, parallel_chunks_per_file × chunk_size / RTT)`
  Past the link cap, more chunks just queue; below it, doubling either
  knob roughly doubles throughput.

- **Split-file Telegram leech**:
  `total_size / (max_part_size / per_part_throughput)` with
  `upload_parallelism` workers in flight. Equivalent form:
  `min(link_bw, upload_parallelism × per_part_throughput)`.

- **Aria2 multi-source HTTP**:
  `min(link_bw, split × per_connection_throughput)`.
  `max_connection_per_server` only matters when `split` exceeds the
  number of distinct mirrors aria2 has resolved.

- **Pipelined mirror (PR 11)**:
  `total_size / max(downstream_throughput, upstream_throughput)`,
  effective once the downloader has crossed the equivalent of rclone's
  `multi_thread_cutoff` so the first chunk is large enough to upload
  while the rest still streams in.

## Tuning playbook

One-line heuristics. Each maps to a config field in
`include/cmlb/core/configuration.hpp`.

### GoogleDrive

- If GDrive uploads plateau at < 25 % of link bw, double
  `google_drive.parallel_chunks_per_file` (4 → 8) and retry.
- If chunk PUTs return HTTP 400/416, drop
  `google_drive.parallel_chunks_per_file` to 1 (sequential fallback).
- If wall time of a directory upload doesn't drop when you raise
  `parallel_files_per_directory`, the bottleneck is the per-file
  uploader, not the dispatch — tune `parallel_chunks_per_file` instead.
- If you see 429 Too Many Requests storms in logs, lower
  `parallel_files_per_directory` and let the backoff loop catch up
  (`max_retries`, `initial_retry_delay`).
- If a single file uploads at full speed but small files are slow, raise
  `google_drive.chunk_size` so small files finish in one chunk.

### Telegram

- If split-file uploads stall, halve `telegram.upload_parallelism` — the
  bot may be hitting FLOOD_WAIT_X on the destination chat.
- If a single 2 GiB part is slow but parallelism is high, raise
  `telegram.upload_chunk_size_kb` (2048 → 4096) — fewer round-trips per
  part.
- If you see "TDLib setOption(X) failed" warnings, ignore — the option
  was renamed/removed in your TDLib build. Not fatal.
- If TDLib reconnects every few minutes on a flaky link, raise
  `telegram.connection_retry_count_max`.

### Aria2

- If aria2 sits at one-stream throughput, you forgot
  `aria2.max_connection_per_server` — it caps `split` for HTTP origins.
- If a 1 GiB file uses < 10 % CPU and < 50 % link, your origin doesn't
  support range requests; nothing to do at the bot layer.
- If aria2 keeps thrashing the disk, raise `aria2.disk_cache` (default
  128M → 256M / 512M).
- If you saturate the link but BitTorrent stays slow, raise
  `aria2.bt_max_peers`.

### qBittorrent

- If qBit downloads stall at high peer counts, lower
  `qbittorrent.max_connections_per_torrent` — most trackers throttle
  beyond 100.
- If `setPreferences` returns 400 for a key, that key isn't supported in
  your qBit build; logged at `warn`, not fatal. Confirm via the qBit
  WebUI which keys it knows.
- If `/qbmirror` consumes all RAM, lower `qbittorrent.disk_cache_mib`.

### Rclone

- If rclone OOMs, lower `rclone.buffer_size` (RAM = `transfers × buffer_size`).
- If small files are slow but large ones are fast, raise
  `rclone.checkers` — listing/hash overhead dominates.
- If multi-thread doesn't trigger, lower `rclone.multi_thread_cutoff`.
- If GDrive remotes specifically are slow, raise `rclone.drive_chunk_size`
  (must be a 256K multiple).
- If you see retries hammering on transient 5xx, leave it — rclone's
  internal backoff handles it. Don't add `--retries` to `rclone.extra_args`.

## Known caveats

- **GDrive parallel chunks against a single session URI** (PR 6) is an
  aggressive use of the resumable upload protocol. Google's docs describe
  it sequentially. If you observe HTTP 400 or 416 from the chunk PUTs in
  `logs/cmlb.log`, set `google_drive.parallel_chunks_per_file: 1` to fall
  back to strictly sequential — single-stream throughput is still good,
  you just lose the fan-out win.
- **TDLib option names can change** across minor versions. If you see
  "TDLib setOption(X) failed" in logs, that option was renamed or removed
  by the TDLib build you're linked against. The bot logs the failure at
  `warn` and continues; no fatal effect. Tracked TDLib options:
  `upload_chunk_size_kb`, `download_chunk_size_kb`,
  `connection_retry_count_max`.
- **qBittorrent `setPreferences` returns 400 for unknown keys** (PR 9).
  Older qBit builds (pre 4.5) reject some keys that newer ones accept.
  These rejections are logged at `warn` and are not fatal — the rest of
  the preferences still apply atomically. If you depend on a specific
  preference, confirm via `GET /api/v2/app/preferences` after login.
- **Pipelined mirror metrics are only meaningful** when both legs are
  bandwidth-limited. If your downstream is 10× your upstream, pipelining
  saves you the upstream's wall time minus the time-to-first-byte of the
  downloader, no more.
- **Aria2 hard caps** `split` and `max_connection_per_server` at 16 in
  default builds. Raising the config above 16 is a no-op.

## Changelog

See [`CHANGELOG.md`](../CHANGELOG.md) for the per-PR breakdown of the
optimization pass (Aria2 tuning, Beast HTTP client + connection pool,
subprocess line-streaming, expanded config structs, rclone arg
generation, GDrive parallel chunks + parallel files, qBit
`setPreferences`, TDLib option push, use-case pipelining, Telegram
split-file parallelism) and the full v1.0.0 baseline that preceded it.
