# CMLB

Production-grade C++23 Telegram mirror/leech bot.

[![CI](https://img.shields.io/github/actions/workflow/status/staneswilson/cpp-mirror-leech-bot/ci.yml?branch=main&label=CI)](https://github.com/staneswilson/cpp-mirror-leech-bot/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.0--alpha-blue)](VERSION)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)

---

## Table of contents

- [What is CMLB?](#what-is-cmlb)
- [Feature matrix](#feature-matrix)
- [Quick start](#quick-start)
  - [Docker](#docker)
  - [Pre-built binary](#pre-built-binary)
  - [Build from source](#build-from-source)
- [Configuration](#configuration)
- [Commands](#commands)
- [Architecture](#architecture)
- [Development](#development)
- [License](#license)
- [Acknowledgements](#acknowledgements)

---

## What is CMLB?

CMLB is a Telegram bot that turns a chat into a remote control for a download-and-upload pipeline. Send it a URL, magnet link, `.torrent` file, or RSS feed and CMLB will fetch the content, optionally repackage or transcode it, and deliver the result back into Telegram or out to cloud storage. It is the C++ successor to the popular Python mirror-leech bots, rebuilt from scratch with stricter typing, deterministic resource management, and an architecture that is meant to be auditable, not just functional.

The problem CMLB solves is operational: hobbyist mirror bots tend to be a single tangle of global state, ad-hoc subprocess management, and best-effort error handling. They work until they don't, and when they break the failure modes are opaque. CMLB approaches the same workflow as a long-running enterprise service. Every external I/O call goes through a typed adapter, every result is a `Result<T>` with a structured error code, every long task is cancellable through Asio cancellation slots, and every persistent fact lives in SQLite with versioned migrations rather than in ephemeral memory.

What makes CMLB different from existing C++ Telegram bots is the discipline around isolation. Telegram is reached through TDLib, but only one file in the entire codebase is allowed to include `<td/telegram/td_api.h>` — the `TelegramGateway`. Downloads run through `aria2c` over WebSocket JSON-RPC or qBittorrent over its Web API, but the rest of the codebase only sees the `DownloaderInterface`. Uploads target Telegram, Google Drive (service-account JWT), or rclone remotes, but the use cases only know `UploaderInterface`. This layered design is enforced by clang-tidy, include-what-you-use, and a CI matrix that compiles cleanly on GCC 14, Clang 20 (with ASan / UBSan / TSan), MSVC 2022, and Apple Clang — all four with warnings-as-errors.

---

## Feature matrix

| Capability | Status | Notes |
|---|---|---|
| Mirror from direct URL | Supported | Uses aria2c; resumable; rate-limited per user tier |
| Leech to Telegram | Supported | Streaming upload; auto-split at configurable size; 4 GB premium support |
| qBittorrent torrent / magnet | Supported | Web API client; seeding policy configurable |
| aria2 torrent / magnet | Supported | WebSocket JSON-RPC with backpressure |
| Google Drive upload | Supported | Service-account JWT; resumable session uploads |
| Google Drive clone / count / delete | Supported | `/clone`, `/count`, `/del` |
| rclone remote upload | Supported | Subprocess wrapper around `rclone copy` |
| RSS subscriptions | Supported | Polled; per-feed regex filter; deduplicated by GUID |
| Archive extract (7z, zip, tar, rar) | Supported | Via libarchive + 7z fallback |
| Archive compress (7z) | Supported | Configurable split-volume size |
| Media thumbnail / sample / metadata | Supported | ffmpeg subprocess |
| YouTube-DL / yt-dlp integration | Supported | Subprocess wrapper |
| Permission tiers | Supported | Anyone / User / Admin / Owner |
| Per-user settings | Supported | Stored in SQLite |
| Cancellable tasks | Supported | Asio cancellation slots; `/cancel` and `/cancelall` |
| Pause / resume tasks | Supported | Downloader-level; not all downloaders |
| Live progress edits | Supported | Throttled message edits; configurable interval |
| Status dashboard | Supported | `/status` shows all active tasks |
| Stats and uptime | Supported | `/stats` with system metrics |
| Prometheus metrics | Optional | Off by default; opt-in via config |
| Web UI | Not in v1 | Reserved for v2 |
| Multi-bot mode | Not in v1 | One TDLib instance per process |

---

## Quick start

CMLB ships three supported installation paths. Pick the one that matches your environment.

### Docker

The fastest path. The published image bundles a known-good build of TDLib, aria2c, ffmpeg, 7z, and rclone.

```bash
# 1. Clone the repository (you need the compose file and template config)
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb

# 2. Copy the example config and fill in your credentials
cp config.example.json config.json
$EDITOR config.json   # set telegram.api_id, api_hash, bot_token, owner_id

# 3. Start the bot
docker compose up -d

# 4. Tail logs
docker compose logs -f cmlb
```

The compose file mounts `./config.json`, `./data/`, and `./downloads/` as volumes so state survives container recreation. The persistent footprint is small — typically a few MB of SQLite plus whatever you choose to keep under `downloads/`.

### Pre-built binary

For Linux x86_64 hosts, a static-ish binary is published per release.

```bash
curl -fsSL https://github.com/staneswilson/cpp-mirror-leech-bot/releases/latest/download/install.sh | bash

# install.sh drops the binary at /usr/local/bin/cmlb and a systemd unit at
# /etc/systemd/system/cmlb.service. Then:
sudo systemctl edit cmlb           # set CMLB_CONFIG_PATH if non-default
sudo systemctl enable --now cmlb
journalctl -u cmlb -f
```

The installer never modifies your config or data directories. If `cmlb.service` already exists it is left alone; the installer only refreshes the binary.

### Build from source

If you want full control, or your platform isn't covered by the prebuilt artifacts, build from source. This is also the path you'll use for development.

```bash
# Toolchain prerequisites (Linux)
sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip pkg-config \
    libssl-dev zlib1g-dev gperf

# vcpkg (manifest mode; CMLB pins versions in vcpkg.json)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg

# Clone and build
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb
cmake --preset release
cmake --build --preset release

# Run from the build tree
./build/release/cmlb --config config.json
```

> **Note:** The first build compiles TDLib from source via vcpkg. Plan for 15-30 minutes on a 4-core machine and 4-8 GB of free RAM. Subsequent builds reuse the vcpkg binary cache and finish in seconds.

For the full build matrix (debug, asan, ubsan, tsan, coverage, MSVC) see [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## Configuration

CMLB is configured through a single JSON file (default: `./config.json`). Every field can be overridden by an environment variable using the `CMLB_<UPPER_SNAKE>` convention — for example `telegram.api_id` becomes `CMLB_TELEGRAM_API_ID`.

A minimal viable config:

```json
{
  "telegram": {
    "api_id": 0,
    "api_hash": "REPLACE_ME",
    "bot_token": "REPLACE_ME:AAFAKEFAKE",
    "owner_id": 0
  },
  "aria2": {
    "rpc_url": "ws://127.0.0.1:6800/jsonrpc",
    "rpc_secret": "REPLACE_ME"
  },
  "paths": {
    "downloads": "./downloads",
    "data": "./data"
  },
  "logging": {
    "level": "info",
    "file": "./data/cmlb.log"
  }
}
```

For every field, default, and validation rule see [`docs/configuration_reference.md`](docs/configuration_reference.md).

> **Warning:** Never commit `config.json`, `service_account.json`, or `tdlib/` to source control. The `.gitignore` shipped with CMLB already excludes them; do not weaken those rules.

---

## Commands

A handful of headline commands; the full reference with permissions and examples lives in [`docs/command_reference.md`](docs/command_reference.md).

| Command | One-line description |
|---|---|
| `/start` | Show greeting and check authorization |
| `/help` | Show grouped command list |
| `/mirror <url>` | Download to disk and upload to the configured cloud destination |
| `/leech <url>` | Download and re-upload back into the current Telegram chat |
| `/qbmirror <url\|magnet>` | Same as `/mirror`, but force the qBittorrent downloader |
| `/qbleech <url\|magnet>` | Same as `/leech`, but force the qBittorrent downloader |
| `/clone <gdrive-link>` | Server-side clone of a Google Drive resource |
| `/count <gdrive-link>` | Recursively count files and total bytes in a Drive folder |
| `/del <gdrive-link>` | Delete a Drive resource owned by the service account |
| `/status` | Show all active tasks with live progress |
| `/cancel <task-id>` | Cancel a single task |
| `/cancelall` | Cancel every task owned by the caller |
| `/pause <task-id>` | Pause a paused-capable task |
| `/resume <task-id>` | Resume a paused task |
| `/settings` | Open the per-user settings panel (inline keyboard) |
| `/botsettings` | Owner-only: edit bot-wide settings live |
| `/stats` | Show system stats: CPU, RAM, disk, uptime, task counts |
| `/ping` | Health check; replies with round-trip time |
| `/log` | Owner-only: tail the last N log lines |
| `/rss add\|list\|remove` | Manage RSS subscriptions |

---

## Architecture

CMLB is laid out as a strict five-layer Domain-Driven Design: `core/` provides primitives (`Result<T>`, `Logger`, `Executor`), `domain/` holds the business model (`Task` aggregate, `Authority`, strong-typed identifiers), `application/` contains verb-noun use cases (`MirrorUrl`, `LeechUrl`, `CancelTask`, ...), `infrastructure/` houses every external adapter (TDLib, aria2, qBittorrent, SQLite, Google Drive, rclone, ffmpeg, 7z), and `presentation/` deals with the Telegram command surface (parser, dispatcher, renderers). Dependencies point inwards only — `infrastructure` may depend on `application`, but never the reverse.

The async model is Boost.Asio C++20 coroutines: every external call returns `awaitable<Result<T>>`, runs on the shared `io_context`, and is cancellable through Asio cancellation slots. There is no manual job queue and no thread-pool task scheduler invented in-house.

For the full layer breakdown, dataflow diagrams, error taxonomy, and the rationale behind each major decision see [`docs/architecture.md`](docs/architecture.md) and the [Architecture Decision Records](docs/adr/).

---

## Development

Contributing changes? Start with [`CONTRIBUTING.md`](CONTRIBUTING.md) for developer setup, coding standards, and PR workflow. For operational tasks — deploying, upgrading, backups, troubleshooting — see [`docs/runbook.md`](docs/runbook.md).

The short version:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

CI runs the same presets on Linux GCC 14, Linux Clang 20 (with ASan, UBSan, TSan in separate jobs), Windows MSVC 2022, and macOS Clang. Warnings-as-errors is symmetric across all four toolchains; see [ADR-0005](docs/adr/0005-warnings-as-errors-symmetric.md).

---

## License

CMLB is released under the MIT License. See [`LICENSE`](LICENSE) for the full text.

---

## Acknowledgements

CMLB stands on the shoulders of giants. In rough order of how much code we'd have to write without them:

- **[TDLib](https://github.com/tdlib/td)** — the official cross-platform Telegram client library. CMLB uses its JSON interface through a single isolated gateway.
- **[aria2](https://aria2.github.io/)** — the multi-protocol download utility that does the actual fetching for most workflows.
- **[qBittorrent](https://www.qbittorrent.org/)** — used in `nox` (headless) mode as an alternative torrent client when aria2 isn't the right fit.
- **[Boost](https://www.boost.org/)** — Asio, Beast, JSON, Process. The async runtime, HTTP client, and JSON parser are all Boost.
- **[SQLite](https://sqlite.org/)** and **[sqlite-modern-cpp](https://github.com/SqliteModernCpp/sqlite_modern_cpp)** — embedded persistence with a clean C++ API.
- **[spdlog](https://github.com/gabime/spdlog)** and **[fmt](https://github.com/fmtlib/fmt)** — logging and formatting.
- **[Catch2](https://github.com/catchorg/Catch2)** and **[RapidCheck](https://github.com/emil-e/rapidcheck)** — unit testing and property-based testing.
- **[ffmpeg](https://ffmpeg.org/)**, **[7-Zip](https://www.7-zip.org/)**, **[rclone](https://rclone.org/)** — the subprocess workhorses for media, archives, and remote storage.

Thanks also to the maintainers of the original Python mirror-leech projects whose feature set inspired this rewrite.
