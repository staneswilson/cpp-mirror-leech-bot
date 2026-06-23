# CMLB

Production-grade C++23 Telegram mirror/leech bot.

[![CI](https://img.shields.io/github/actions/workflow/status/staneswilson/cpp-mirror-leech-bot/continuous_integration.yml?branch=main&label=CI)](https://github.com/staneswilson/cpp-mirror-leech-bot/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-blue)](VERSION)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)

---

## Table of contents

- [What is CMLB?](#what-is-cmlb)
- [Feature matrix](#feature-matrix)
- [Quick start](#quick-start)
  - [Docker](#docker)
  - [Release artifacts](#release-artifacts)
  - [Build from source](#build-from-source)
- [Documentation and wiki](#documentation-and-wiki)
- [Configuration](#configuration)
- [Commands](#commands)
- [Packages and images](#packages-and-images)
- [Architecture](#architecture)
- [Development](#development)
- [License](#license)
- [Acknowledgements](#acknowledgements)

---

## What is CMLB?

CMLB is a Telegram bot that turns a chat into a remote control for a
download-and-upload pipeline. Send it a URL, magnet link, torrent URL, or RSS
feed and CMLB will fetch the content and deliver the result back into Telegram
or out to cloud storage. It is the C++ successor to the popular Python
mirror-leech bots, rebuilt from scratch with stricter typing, deterministic
resource management, and an architecture that is meant to be auditable, not
just functional.

The problem CMLB solves is operational: hobbyist mirror bots tend to be a single tangle of global state, ad-hoc subprocess management, and best-effort error handling. They work until they don't, and when they break the failure modes are opaque. CMLB approaches the same workflow as a long-running enterprise service. Every external I/O call goes through a typed adapter, every result is a `Result<T>` with a structured error code, every long task is cancellable through Asio cancellation slots, and every persistent fact lives in SQLite with versioned migrations rather than in ephemeral memory.

What makes CMLB different from existing C++ Telegram bots is the discipline around isolation. Telegram is reached through TDLib, but only one file in the entire codebase is allowed to include `<td/telegram/td_api.h>`: the `TelegramGateway`. Downloads run through `aria2c` over WebSocket JSON-RPC or qBittorrent over its Web API, but the rest of the codebase only sees the `DownloaderInterface`. Uploads target Telegram, Google Drive (service-account JWT), or rclone remotes, but the use cases only know `UploaderInterface`. This layered design is enforced by clang-tidy, a CI TDLib include-isolation probe, CMake target visibility, and a CI matrix that compiles cleanly on GCC 14, Clang 20 (with ASan / UBSan / TSan), MSVC 2022, and Apple Clang. All four toolchains build with warnings-as-errors.

---

## Feature matrix

| Capability | Status | Notes |
|---|---|---|
| Mirror from direct URL | Supported | Uses aria2c; resumable; cancellable |
| Leech to Telegram | Supported | Streaming upload; auto-split at the bot-wide split size |
| qBittorrent torrent / magnet | Supported | Web API client; seeding policy configurable |
| aria2 torrent / magnet | Supported | WebSocket JSON-RPC with backpressure |
| Google Drive upload | Supported | Service-account JWT; resumable session uploads |
| Google Drive clone / count / delete | Supported | `/clone`, `/count`, `/del` |
| rclone remote upload | Supported | Subprocess wrapper around `rclone copy` |
| RSS subscriptions | Supported | Polled; per-feed regex filter; deduplicated by GUID |
| Archive extract / compress | Adapter ready | 7z-backed processor exists; command wiring is planned |
| Media thumbnail / sample / metadata | Adapter ready | ffmpeg-backed processor exists; command wiring is planned |
| YouTube-DL / yt-dlp integration | Roadmap | Not part of the current v1 command surface |
| Permission tiers | Supported | Anyone / User / Admin / Owner |
| Per-user settings | Supported | Stored in SQLite |
| Cancellable tasks | Supported | Asio cancellation slots; `/cancel` and `/cancelall` |
| Pause / resume tasks | Supported | Downloader-level; not all downloaders |
| Live progress edits | Supported | Throttled message edits with content deduplication |
| Status dashboard | Supported | `/status` shows all active tasks |
| Stats and uptime | Supported | `/stats` with system metrics |
| Prometheus metrics | Roadmap | `/status` and `/stats` expose host metrics today; a scrape endpoint is not shipped yet |
| Web UI | Not in v1 | Reserved for v2 |
| Multi-bot mode | Not in v1 | One TDLib instance per process |

---

## Quick start

CMLB ships three supported installation paths. Pick the one that matches your
environment. If you want the shortest copy/paste path, use the
[deployment quickstart](docs/deployment_quickstart.md).

### Docker

The fastest path. The published image bundles a known-good CMLB build with
TDLib and runtime tools; the Compose stack starts aria2 as the downloader
sidecar.

```bash
# 1. Clone the repository (you need the compose file and template config)
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb

# 2. Copy the example config and compose environment, then fill in credentials
cp config.example.json config.json
cp packaging/.env.example packaging/.env
$EDITOR config.json      # set telegram.api_id, api_hash, bot_token, owner_id
$EDITOR packaging/.env   # set TELEGRAM_*, OWNER_ID, ARIA2_SECRET, CMLB_TAG

# 3. Start the bot from a published image
docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb aria2
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build

# Or build this checkout locally instead:
# docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --build

# 4. Tail logs
docker compose -f packaging/docker-compose.yml --env-file packaging/.env logs -f cmlb
```

The compose file mounts `./config.json`, `cmlb-data`, `cmlb-logs`, and the shared aria2 download volume so state survives container recreation. Production deployments must pin `CMLB_TAG` to a release tag or immutable `sha-<full commit>` image tag; the example intentionally does not default to `latest`.

### Release artifacts

Tagged releases publish binaries and container images from `.github/workflows/release.yml` and `.github/workflows/docker.yml`. Until a trusted `vX.Y.Z` tag exists, use the Docker compose path above or build from source.

```bash
gh release list --repo staneswilson/cpp-mirror-leech-bot
gh run list --workflow docker --branch main --repo staneswilson/cpp-mirror-leech-bot
```

The release workflow is tag-driven so production upgrades are explicit. Do not deploy a floating `latest` tag for long-lived hosts.

### Build from source

If you want full control, or your platform isn't covered by the prebuilt artifacts, build from source. This is also the path you'll use for development.

```bash
# Toolchain prerequisites (Linux)
sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip pkg-config \
    gcc-14 g++-14 libssl-dev zlib1g-dev gperf autoconf autoconf-archive \
    automake libtool bison flex python3 python3-jinja2 ca-certificates \
    tar xz-utils bzip2

# vcpkg (manifest mode; CMLB pins the registry baseline in both manifest files)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
export CC=gcc-14 CXX=g++-14

# Clone and build
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb
cmake --preset release
cmake --build --preset release

# Run from the build tree
./build/release/bin/cmlb config.json
```

> **Note:** The first build compiles TDLib from source via vcpkg. Plan for 15-30 minutes on a 4-core machine and 4-8 GB of free RAM. Subsequent builds reuse the vcpkg binary cache and finish in seconds.

For the full build matrix (debug, asan, ubsan, tsan, coverage, MSVC) see [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## Documentation and wiki

The documentation is organized for operators first:

| Need | Read |
|---|---|
| Deploy from an empty Linux machine | [`docs/deployment_quickstart.md`](docs/deployment_quickstart.md) |
| Operate, upgrade, back up, restore, and troubleshoot | [`docs/runbook.md`](docs/runbook.md) |
| Set every config field and environment variable | [`docs/configuration_reference.md`](docs/configuration_reference.md) |
| Register and use Telegram commands | [`docs/command_reference.md`](docs/command_reference.md) |
| Understand architecture and CI guardrails | [`docs/architecture.md`](docs/architecture.md) and [`docs/adr/`](docs/adr/) |

The GitHub Wiki is enabled for the repository. The wiki publishing map is in
[`docs/wiki.md`](docs/wiki.md); until GitHub initializes the wiki git
repository, these versioned docs remain the source of truth.

---

## Configuration

CMLB is configured through a single JSON file (default: `./config.json` if no
path argument is supplied). Supported fields can be overridden by `CMLB_*`
environment variables; for example `telegram.api_id` becomes
`CMLB_TELEGRAM_API_ID`.

A minimal viable config:

```json
{
  "telegram": {
    "api_id": 123456,
    "api_hash": "REPLACE_ME",
    "bot_token": "REPLACE_ME:AAFAKEFAKE",
    "owner_id": 987654321
  },
  "aria2": {
    "rpc_url": "ws://127.0.0.1:6800/jsonrpc",
    "secret": "REPLACE_ME"
  },
  "paths": {
    "download_dir": "./downloads",
    "data_dir": "./data"
  },
  "logging": {
    "level": "info",
    "logs_dir": "./data/logs",
    "console": true
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
| `/start` | Show greeting and confirm the bot is alive |
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
| `/cancelall` | Admin-only: cancel every active task in the current chat |
| `/pause <task-id>` | Pause a paused-capable task |
| `/resume <task-id>` | Resume a paused task |
| `/settings` | Open the per-user settings panel (inline keyboard) |
| `/botsettings` | Admin-only: show the bot-wide settings panel |
| `/stats` | Show system stats: CPU, RAM, disk, uptime, task counts |
| `/ping` | Health check; replies with round-trip time |
| `/version` | Show the running CMLB build version |
| `/log` | Owner-only: upload the current `logs/cmlb.log` file |
| `/rss add\|list\|remove` | Manage RSS subscriptions |

---

## Packages and Images

Docker is the primary production package. Images are published to
`ghcr.io/staneswilson/cpp-mirror-leech-bot` by the Docker workflow for
`linux/amd64` and `linux/arm64`.

| Artifact | Status | Production guidance |
|---|---|---|
| GHCR image `sha-<full commit>` | Supported | Preferred: immutable and reproducible |
| GHCR image `vX.Y.Z` | Supported for trusted release tags | Good for planned upgrades |
| GHCR image `main` / `latest` | Published for convenience | Avoid for long-lived hosts |
| CPack `.tar.gz` / `.zip` | Supported release artifacts | Use for manual/systemd installs |
| Native `.deb` / `.rpm` | Not published | Deferred until package metadata is CI-validated |

Dependency resolution is pinned through `vcpkg.json`,
`vcpkg-configuration.json`, and the Docker build argument
`VCPKG_BASELINE=a7eda31dc16994fcaa8587982eb833a8695f1b6f`. The runtime image
also pins upstream rclone and 7-Zip binaries instead of relying on stale distro
packages. Do not replace those pins with floating package manager state in
deployment automation.

---

## Architecture

CMLB is laid out as a strict five-layer Domain-Driven Design: `core/` provides primitives (`Result<T>`, `Logger`, `Executor`), `domain/` holds the business model (`Task` aggregate, `Authority`, strong-typed identifiers), `application/` contains verb-noun use cases (`MirrorUrl`, `LeechUrl`, `CancelTask`, ...), `infrastructure/` houses every external adapter (TDLib, aria2, qBittorrent, SQLite, Google Drive, rclone, ffmpeg, 7z), and `presentation/` deals with the Telegram command surface (parser, dispatcher, renderers). Dependencies point inwards only — `infrastructure` may depend on `application`, but never the reverse.

The async model is Boost.Asio C++20 coroutines: every external call returns `awaitable<Result<T>>`, runs on the shared `io_context`, and is cancellable through Asio cancellation slots. There is no manual job queue and no thread-pool task scheduler invented in-house.

For the full layer breakdown, dataflow diagrams, error taxonomy, and the rationale behind each major decision see [`docs/architecture.md`](docs/architecture.md) and the [Architecture Decision Records](docs/adr/).

---

## Development

Contributing changes? Start with [`CONTRIBUTING.md`](CONTRIBUTING.md) for developer setup, coding standards, and PR workflow. For operational tasks — deploying, upgrading, backups, troubleshooting — see [`docs/runbook.md`](docs/runbook.md). The documentation index is [`docs/README.md`](docs/README.md).

The short version:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

CI runs the same presets on Linux GCC 14, Linux Clang 20 (with ASan, UBSan, TSan in separate jobs), Windows MSVC 2022, and macOS Clang. Warnings-as-errors is symmetric across all four toolchains; see [ADR-0005](docs/adr/0005-warnings-as-errors-symmetric.md).

Package resolution is pinned, not floating. `vcpkg.json`, `vcpkg-configuration.json`, and `packaging/Dockerfile` all use baseline `a7eda31dc16994fcaa8587982eb833a8695f1b6f`; Docker images are published as `ghcr.io/staneswilson/cpp-mirror-leech-bot:<tag>`.

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
