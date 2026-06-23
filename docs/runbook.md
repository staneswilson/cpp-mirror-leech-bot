# CMLB operator runbook

This document is for the person who runs CMLB in production. It assumes you are comfortable on a Linux shell, with systemd, and with reading log files. If you are looking for how to *develop* CMLB, read [`CONTRIBUTING.md`](../CONTRIBUTING.md). If you are looking for what each config field means, read [`configuration_reference.md`](configuration_reference.md).

---

## Table of contents

- [Prerequisites](#prerequisites)
- [Initial setup](#initial-setup)
- [Packages and images](#packages-and-images)
- [Configuration](#configuration)
- [First run](#first-run)
- [Updating](#updating)
- [Backups](#backups)
- [Restoring from backup](#restoring-from-backup)
- [Common operational tasks](#common-operational-tasks)
  - [Changing log level](#changing-log-level)
  - [Rotating credentials](#rotating-credentials)
  - [Graceful shutdown / draining](#graceful-shutdown--draining)
- [Troubleshooting](#troubleshooting)
- [Performance tuning](#performance-tuning)
- [Security checklist](#security-checklist)

---

## Prerequisites

**Operating systems supported in production:**

- Linux x86_64 — primary target. CI builds on Ubuntu 24.04.
- Linux aarch64 — supported by the Docker workflow for container images. Native release binaries are not currently published.
- Windows — supported for development. Production deployments are not recommended (TDLib session locking, filesystem case-sensitivity edge cases).
- macOS — supported for development. Not a production target.

**Toolchain (build from source only):**

- A C++23 compiler: GCC 14, Clang 20, MSVC 2022, or Apple Clang on the current macOS runner.
- CMake 3.28 or newer.
- Ninja (the default generator).
- `git`, `curl`, `ca-certificates`, `zip`, `unzip`, `tar`, `pkg-config`, `gperf`.
- vcpkg port helpers: `autoconf`, `autoconf-archive`, `automake`,
  `libtool`, `bison`, `flex`, `python3`, `python3-jinja2`.
- 4+ GB of free RAM during the first build.
- 8+ GB of free disk space for build artifacts and the vcpkg cache.

**External services that must be reachable at runtime:**

- Telegram (via TDLib): `api.telegram.org`, `*.t.me`, the DC IPs TDLib selects.
- aria2 RPC, if you use aria2 (default: `ws://127.0.0.1:6800/jsonrpc`).
- qBittorrent Web API, if you use qBittorrent (default: `http://127.0.0.1:8080`).
- Google APIs (`oauth2.googleapis.com`, `www.googleapis.com`), if you use Drive.
- The remote hosts you intend to download from.

> **Note:** The first build of TDLib through vcpkg takes 15-30 minutes and 4-8 GB of RAM on a 4-core machine. Run the initial build on a host with adequate resources or expect to swap heavily. Subsequent builds use the cached binary.

---

## Initial setup

### Fast Docker Deployment

This is the shortest production-shaped path. It uses the checked-in compose
file, a pinned aria2 image digest, and a pinned CMLB image tag you choose. For
a beginner-friendly walkthrough with BotFather prompts translated into exact
values, read [`deployment_quickstart.md`](deployment_quickstart.md).

**You need before starting:**

| Item | Where to get it |
|---|---|
| `telegram.api_id` and `telegram.api_hash` | <https://my.telegram.org/apps> |
| `telegram.bot_token` | Telegram BotFather, `/newbot` |
| `telegram.owner_id` | Telegram `@userinfobot` |
| `ARIA2_SECRET` | Generate locally with `openssl rand -hex 32` |
| `CMLB_TAG` | A release tag, `sha-<full commit>` from the Docker workflow, or a local build tag |

```bash
# 1. Install Docker and the Compose plugin using your distro package manager.
docker version
docker compose version

# 2. Clone CMLB.
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb

# 3. Create local config files. These are gitignored.
cp config.example.json config.json
cp packaging/.env.example packaging/.env

# 4. Fill in Telegram credentials and owner id. Compose injects these as
#    environment overrides, so config.json can stay as the template for a
#    minimal Telegram + aria2 deployment.
$EDITOR packaging/.env

# 5. Validate the compose file before starting containers.
docker compose -f packaging/docker-compose.yml --env-file packaging/.env config >/tmp/cmlb-compose.yml

# 6a. Preferred production path: pull the pinned image and start without a
#     local rebuild.
docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb aria2
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build

# 6b. Local image path: compile this checkout into the image.
# docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --build

# 7. Confirm both services are healthy and follow logs.
docker compose -f packaging/docker-compose.yml ps
docker compose -f packaging/docker-compose.yml logs -f cmlb
```

Then open Telegram and send `/start` to the bot. If the bot does not reply,
check `docker compose ... logs cmlb` first; configuration errors are printed
before the bot connects to Telegram.

Production rule: do not deploy floating tags (`latest`, `main`). Use a trusted
release tag or the immutable `sha-<full commit>` tag produced by the Docker
workflow.

### Source Build Deployment

Use this when you need a local binary, custom packaging, or a platform not
covered by the published image.

```bash
# 1. Create a service user (recommended for systemd installs)
sudo useradd -r -s /usr/sbin/nologin -d /var/lib/cmlb -m cmlb

# 2. Clone the repository
sudo -u cmlb git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git /var/lib/cmlb/src
cd /var/lib/cmlb/src

# 3. On Ubuntu 24.04, install the source-build package set if you have not
# already run scripts/bootstrap_linux.sh:
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential gcc-14 g++-14 cmake ninja-build git curl ca-certificates \
    pkg-config autoconf autoconf-archive automake libtool bison flex gperf \
    zip unzip tar xz-utils bzip2 python3 python3-jinja2 libssl-dev zlib1g-dev

# 4. Install vcpkg (manifest mode; do not modify vcpkg.json by hand)
sudo -u cmlb git clone https://github.com/microsoft/vcpkg.git /var/lib/cmlb/vcpkg
sudo -u cmlb /var/lib/cmlb/vcpkg/bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT=/var/lib/cmlb/vcpkg' | sudo tee /etc/profile.d/cmlb.sh

# 5. Build the release preset
sudo -u cmlb env VCPKG_ROOT=/var/lib/cmlb/vcpkg CC=gcc-14 CXX=g++-14 \
    cmake --preset release
sudo -u cmlb env VCPKG_ROOT=/var/lib/cmlb/vcpkg CC=gcc-14 CXX=g++-14 \
    cmake --build --preset release -j

# 6. Install the binary system-wide (or run from the build tree)
sudo install -m 0755 build/release/bin/cmlb /usr/local/bin/cmlb

# 7. Verify the binary
/usr/local/bin/cmlb --version
```

For systemd installs, copy `packaging/systemd/cmlb.service` to
`/etc/systemd/system/cmlb.service`, then adjust paths if your binary or config
does not live at `/usr/local/bin/cmlb` and `/etc/cmlb/config.json`.

---

## Packages and images

Use Docker for the simplest production deployment. The Docker workflow publishes
multi-architecture images to GitHub Container Registry as
`ghcr.io/staneswilson/cpp-mirror-leech-bot:<tag>`.

Supported runtime tags:

| Tag style | Use |
|---|---|
| `sha-<full commit>` | Preferred for production because it is immutable. |
| `vX.Y.Z` | Release deployments once a trusted release tag exists. |
| `main` / `latest` | Convenience tags only; do not pin long-lived hosts to them. |

Release archives are generated by CPack as `.tar.gz` / `.tgz` and `.zip`
artifacts. Native Debian/RPM packages are intentionally not published until the
service-manager scripts and dependency metadata are validated end-to-end in CI.

To see the currently published package tags:

```bash
gh run list --workflow docker --branch main --repo staneswilson/cpp-mirror-leech-bot
gh api /users/staneswilson/packages/container/cpp-mirror-leech-bot/versions \
  --jq '.[0:10][] | {name, tags: .metadata.container.tags}'
```

When deploying with Compose, put the chosen immutable tag in
`packaging/.env` as `CMLB_TAG=sha-<full commit>`.

---

## Configuration

CMLB reads its configuration from `config.json`. The full schema is in [`configuration_reference.md`](configuration_reference.md). This section covers only what an operator must obtain before first run.

### Obtaining Telegram credentials

You need two pieces of identity:

1. **`telegram.api_id`** and **`telegram.api_hash`** — these identify *your application* to Telegram, not your bot. Get them from <https://my.telegram.org/apps>. Sign in with the phone number that will own the application, create an app if one does not exist, and copy `App api_id` and `App api_hash`. Put them in `config.json` or `TELEGRAM_API_ID` / `TELEGRAM_API_HASH` in `packaging/.env`. **Keep `api_hash` secret.**

2. **`telegram.bot_token`** — this identifies the bot account. Open Telegram, talk to [@BotFather](https://t.me/BotFather), send `/newbot`, follow the prompts. BotFather replies with a token of the form `123456789:AAH...`. Paste it into `config.json` or `packaging/.env` for Compose deployments. **Keep the bot token secret** — anyone with it can impersonate the bot.

3. **`telegram.owner_id`** — your numeric Telegram user id. Talk to [@userinfobot](https://t.me/userinfobot) to retrieve it. Put it in `config.json` or `OWNER_ID` in `packaging/.env`. The owner has unrestricted access including `/log` and `/botsettings`.

> **Note:** v1 supports bot-token authentication only. User-account login is
> not part of the current deployment surface.

### Registering BotFather commands

Telegram clients show a bot command menu only after BotFather knows the command
list. Talk to [@BotFather](https://t.me/BotFather), send `/setcommands`, choose
the bot, then paste:

```text
start - Start CMLB
help - Show available commands
mirror - Mirror a URL or magnet to cloud storage
leech - Download and upload back to Telegram
qbmirror - Mirror with qBittorrent
qbleech - Leech with qBittorrent
clone - Clone a Google Drive file or folder
count - Count Google Drive files and size
del - Delete a Google Drive resource
status - Show task status
cancel - Cancel a task
cancelall - Admin: cancel all tasks in this chat
pause - Pause a task
resume - Resume a task
settings - Open user settings
botsettings - Admin: view bot settings
stats - Show system stats
ping - Check bot latency
version - Show build version
log - Owner: upload current log file
rss - Manage RSS subscriptions
```

BotFather profile polish such as `/setdescription`, `/setabouttext`, and
`/setuserpic` is optional. It does not change CMLB's permissions or command
dispatcher.

### Obtaining a Google service account (optional)

Only required if you use Google Drive for upload, clone, count, or delete.

1. In Google Cloud Console, create a project.
2. Enable the Drive API on that project.
3. Create a *service account*, give it a name, no roles.
4. Generate a JSON key for the service account; download it.
5. Save the file as `service_account.json` next to `config.json` (or wherever `google_drive.credentials_path` points).
6. **Share the destination Drive folder with the service account email** (`<name>@<project>.iam.gserviceaccount.com`) as Editor. This is the step that's easy to forget and produces 404s.

### Filling the Docker Compose environment

`packaging/.env` is read by Docker Compose, not by CMLB directly. Compose
substitutes those values into `docker-compose.yml`, then the CMLB container
receives the matching `CMLB_*` overrides.

Rules:

- Use `NAME=value`, one value per line.
- Do not put spaces around `=`.
- Do not commit `packaging/.env`.
- Generate `ARIA2_SECRET` with `openssl rand -hex 32`.
- Pin `CMLB_TAG`; avoid `latest` and `main` on production hosts.

Minimum production values:

| `.env` value | Source |
|---|---|
| `TELEGRAM_API_ID` | `App api_id` from `my.telegram.org/apps`. |
| `TELEGRAM_API_HASH` | `App api_hash` from `my.telegram.org/apps`. |
| `TELEGRAM_BOT_TOKEN` | BotFather `/newbot`. |
| `OWNER_ID` | Numeric id from `@userinfobot`. |
| `ARIA2_SECRET` | Locally generated random value. |
| `CMLB_IMAGE` | Usually `ghcr.io/staneswilson/cpp-mirror-leech-bot`. |
| `CMLB_TAG` | Trusted release tag or immutable `sha-<full commit>`. |

### Optional rclone setup

Create and test the remote with rclone first:

```bash
rclone config
rclone lsd remote:
```

Then set:

```json
"rclone": {
  "executable": "rclone",
  "config_path": "/etc/cmlb/rclone.conf"
}
```

For Docker, bind-mount the config file into the CMLB container:

```yaml
      - ../rclone/rclone.conf:/etc/cmlb/rclone.conf:ro
```

CMLB expects an upload target formatted as `remote:path` when the user's mirror
destination is rclone. The current `/settings` panel cycles the destination;
text entry for the rclone path is operator-managed in v1.

### Optional qBittorrent setup

qBittorrent is required only for `/qbmirror` and `/qbleech`. The default Compose
stack starts aria2 but not qBittorrent.

Set the Web UI endpoint and credentials:

```json
"qbittorrent": {
  "url": "http://qbittorrent-host:8080",
  "username": "admin",
  "password": "REPLACE_WITH_WEB_UI_PASSWORD"
}
```

The URL must be reachable from the CMLB process. For Docker deployments, put
qBittorrent on the same Compose network or use a host address that containers
can reach.

### Validating the config

Before starting the bot, validate the configuration:

```bash
cmlb --validate-config /etc/cmlb/config.json
```

This loads the file, applies env overrides, and runs the validator. A non-zero exit code means the config is broken; the report lists every problem at once.

---

## First run

```bash
cmlb /etc/cmlb/config.json
```

What happens on the first start:

1. **Schema migration.** CMLB checks `data/cmlb.db` for the `schema_version` row. If missing or behind, every migration above the current version is applied in order. The first start applies all of them.
2. **TDLib authentication.** TDLib creates `tdlib/` and authenticates with
   `telegram.bot_token`. v1 supports bot-token authentication only.
3. **Adapter startup.** Aria2 and qBittorrent connect lazily on first use.
   Google Drive parses its service-account credentials during startup and marks
   the uploader unavailable if the key is missing or invalid.
4. **Update loop.** CMLB subscribes to TDLib updates and begins processing.

You should see these log lines during a healthy start:

```
[info] CMLB 1.0.0 starting up.
[info] authentication_flow: bot authorised
[info] Bot ready. Awaiting Telegram updates.
```

Send `/start` to your bot to confirm end-to-end.

---

## Updating

```bash
# As the service user, in the source tree:
git fetch origin
git checkout <tag-or-commit>
git submodule update --init --recursive  # only if the project pulled in submodules

# Rebuild
cmake --preset release
cmake --build --preset release

# Apply migrations only (does not start the service)
sudo systemctl stop cmlb
sudo -u cmlb /usr/local/bin/cmlb /etc/cmlb/config.json --migrate-only

# Reinstall the binary and start
sudo install -m 0755 build/release/bin/cmlb /usr/local/bin/cmlb
sudo systemctl start cmlb
sudo journalctl -u cmlb -f
```

`--migrate-only` runs the schema migrator and exits 0 if the database is now at the latest version. Run it before starting the new binary so that any migration failure surfaces while the service is still down rather than during a botched startup.

> **Note:** Migrations are forward-only. There is no `--rollback`. If a migration is broken, fix it forward with a new migration. See [ADR-0002](adr/0002-persistence-sqlite-wal.md) for the rationale.

---

## Backups

The full operational state of CMLB consists of:

| Path | What it is | Back up? |
|---|---|---|
| `data/cmlb.db` | All persistent state: tasks, settings, RSS feeds | **Yes**, always |
| `data/cmlb.db-wal`, `data/cmlb.db-shm` | SQLite WAL sidecars | Capture during a quiesce, otherwise skip |
| `tdlib/` | TDLib encrypted session | Optional — losing it means re-auth, not data loss |
| `config.json` | Bot configuration | **Yes**, separately from runtime data |
| `service_account.json` | Google Drive credentials | **Yes**, separately, with stronger access controls |
| `downloads/` | In-flight downloads | No — ephemeral |
| `${logging.logs_dir}/cmlb.log` and rotated logs | Log history; default `logs/cmlb.log`, Docker `/var/log/cmlb/cmlb.log` | At your discretion |

### Live backup procedure

Because SQLite is in WAL mode, you can take a consistent backup *without stopping the bot* using the SQLite backup API:

```bash
sqlite3 /var/lib/cmlb/data/cmlb.db ".backup '/backups/cmlb-$(date +%F).db'"
```

The `.backup` command coordinates with the writer to produce a self-consistent snapshot. Plain `cp` of `cmlb.db` while the bot is running is **not** safe; the WAL contents would be lost.

### Cold backup procedure

```bash
sudo systemctl stop cmlb
sudo -u cmlb tar -czf /backups/cmlb-$(date +%F).tar.gz \
    -C /var/lib/cmlb data tdlib
sudo systemctl start cmlb
```

A cold tarball captures the WAL sidecars as well, but at the cost of downtime.

Whichever method you pick, automate it. A daily cron or systemd timer is enough.

---

## Restoring from backup

```bash
sudo systemctl stop cmlb

# Restore the database
sudo -u cmlb cp /backups/cmlb-2026-05-18.db /var/lib/cmlb/data/cmlb.db
sudo -u cmlb rm -f /var/lib/cmlb/data/cmlb.db-wal /var/lib/cmlb/data/cmlb.db-shm

# (Optional) Restore the TDLib session
sudo -u cmlb tar -xzf /backups/tdlib-2026-05-18.tar.gz -C /var/lib/cmlb

# Validate before starting
sudo -u cmlb /usr/local/bin/cmlb --validate-config /etc/cmlb/config.json

# Run migrations forward if the backup is older than the current binary
sudo -u cmlb /usr/local/bin/cmlb /etc/cmlb/config.json --migrate-only

sudo systemctl start cmlb
sudo journalctl -u cmlb -f
```

If the TDLib session was not restored, the next start recreates it
automatically with `telegram.bot_token`.

---

## Common operational tasks

### Changing log level

There are two ways. The runtime cost is zero either way; pick the one that fits your deployment.

**1. Config-file change (requires restart):**

Edit `logging.level` in `config.json`:

```json
"logging": { "level": "debug" }
```

Then restart the service:

```bash
sudo systemctl restart cmlb
```

**2. Environment variable (also requires restart):**

```bash
sudo systemctl edit cmlb
# add under [Service]:
# Environment=CMLB_LOGGING_LEVEL=debug
sudo systemctl restart cmlb
```

> **Note:** Live log-level reload is **not** supported in v1. The plumbing for live config changes is significant, and the v1 release scope explicitly excludes it. If you need to capture a debug-level trace without a restart, attach to the process with `strace -f` for syscall-level visibility instead.

Valid levels: `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`.

### Rotating credentials

**Telegram bot token:**

1. Talk to BotFather: `/revoke`, choose the bot, get a new token.
2. Stop the service: `sudo systemctl stop cmlb`.
3. Update `telegram.bot_token` in `config.json` (or the `CMLB_TELEGRAM_BOT_TOKEN` env var).
4. Delete the `tdlib/` directory — the old session is bound to the old token.
5. Validate: `cmlb --validate-config /etc/cmlb/config.json`.
6. Start: `sudo systemctl start cmlb`. The bot will re-authenticate with the new token automatically.

**Google service account key:**

1. In Google Cloud Console, create a new JSON key for the same service account (or a new account if you want to retire the old principal entirely).
2. Place the new file at `google_drive.credentials_path`.
3. Restart the service.
4. In Cloud Console, delete the old key.

**aria2 RPC secret / qBittorrent password:**

Rotate the credential in the downstream tool first, then update the matching field in `config.json`, then restart CMLB.

### Graceful shutdown / draining

CMLB handles `SIGTERM` and `SIGINT` as drain signals:

1. Requests TDLib gateway shutdown.
2. Emits Asio cancellation so running coroutines can stop cooperatively.
3. Waits briefly for the executor to drain.
4. Flushes and shuts down the logger.
5. Lets SQLite close through normal object teardown.

```bash
sudo systemctl stop cmlb         # sends SIGTERM, waits, then SIGKILL after timeout
```

Default systemd `TimeoutStopSec` in the shipped unit is 30 seconds. Increase it if your typical task drain takes longer:

```bash
sudo systemctl edit cmlb
# under [Service]:
# TimeoutStopSec=120
```

Avoid `SIGKILL` except as a last resort. SQLite WAL protects committed data,
but forced termination can leave downloader subprocesses, partial files, or
backend-specific task state that needs operator cleanup on the next start.

---

## Troubleshooting

### aria2c not reachable

Symptom: `/mirror <url>` returns "Downloader unavailable" within seconds; log line like:

```
[error] aria2: connect ws://127.0.0.1:6800/jsonrpc failed: Connection refused
```

Steps:

1. Check `aria2.rpc_url` in `config.json`. Is the host/port correct?
2. Confirm aria2c is actually running:

   ```bash
   pgrep -af aria2c
   ss -ltnp | grep 6800
   ```

3. Start aria2c if needed. Minimal command for matching the default config:

   ```bash
   aria2c --enable-rpc --rpc-listen-all=false --rpc-listen-port=6800 \
          --rpc-secret=YOUR_SECRET_HERE --daemon=true
   ```

4. Verify the secret matches `aria2.secret` in CMLB's config. A mismatch surfaces as `auth: unauthorized` in CMLB logs, not as a connection failure.
5. Test RPC manually:

   ```bash
   curl -s http://127.0.0.1:6800/jsonrpc \
        -d '{"jsonrpc":"2.0","id":"x","method":"aria2.getVersion","params":["token:YOUR_SECRET"]}' | jq .
   ```

### TDLib auth failures

Symptom: bot never reaches `ready`. Log lines like:

```
[error] telegram: AUTH_KEY_UNREGISTERED
```

or

```
[error] telegram: failed to read tdlib database
```

Steps:

1. Stop the service.
2. Check filesystem permissions on `tdlib/`. The service user must own it; the directory should be `0700`.

   ```bash
   sudo ls -lad /var/lib/cmlb/tdlib
   sudo chown -R cmlb:cmlb /var/lib/cmlb/tdlib
   sudo chmod 700 /var/lib/cmlb/tdlib
   ```

3. If the error is `AUTH_KEY_UNREGISTERED` or `SESSION_REVOKED`, the session is dead. Delete `tdlib/` to force re-auth:

   ```bash
   sudo -u cmlb rm -rf /var/lib/cmlb/tdlib
   ```

   On next start, the bot re-authenticates with `telegram.bot_token`.
4. If TDLib refuses to start with a database error, the `tdlib/` directory may have been copied from an incompatible TDLib version. Delete it and re-auth.

### SQLite database is locked

Symptom: log line `database is locked` during writes. In WAL mode this is rare and almost always indicates one of:

1. **Stale lock file.** A previous instance crashed without releasing locks.

   ```bash
   sudo systemctl stop cmlb
   sudo -u cmlb ls -la /var/lib/cmlb/data/
   # Look for cmlb.db-wal, cmlb.db-shm, and -journal files
   # The -wal and -shm files are normal; -journal is not
   sudo -u cmlb rm -f /var/lib/cmlb/data/cmlb.db-journal
   sudo systemctl start cmlb
   ```

2. **Two instances pointing at the same database.** Check for stray processes:

   ```bash
   pgrep -af cmlb
   ```

   Only one `cmlb` should be running per database file. Stop the duplicate.

3. **Filesystem doesn't support advisory locks.** This affects some network filesystems (NFS without `nolock=off`). Move `data/` to a local filesystem.

### Google Drive returns 401 / 403

Symptom: `/mirror` to Drive fails with `External authentication rejected` or `External quota exceeded`. Log line:

```
[error] gdrive: 401 invalid_grant
```

or

```
[error] gdrive: 403 storageQuotaExceeded
```

Steps:

1. Confirm `google_drive.credentials_path` points at a real, parseable JSON key.
2. Confirm the Drive API is enabled on the Cloud project that issued the key.
3. Confirm the destination folder is shared with the service-account email as **Editor**. This is the most common cause; the service account cannot see folders that haven't been explicitly shared.
4. Confirm the scope is correct: CMLB requests `https://www.googleapis.com/auth/drive`. If you're using a domain-wide delegated key, ensure that scope is whitelisted in the Workspace admin console.
5. For `storageQuotaExceeded`: service accounts have no personal storage. Either upload to a *Shared Drive* (recommended) or to a folder owned by a real account that you've shared with the service account.
6. Check the clock. JWT signing fails if the system clock is more than 5 minutes off UTC. `chrony` or `systemd-timesyncd` should be running.

---

## Performance tuning

CMLB's defaults are conservative. The fields below can be tuned upward when the host has spare capacity.

| Setting | Default | When to raise | Risk of raising |
|---|---|---|---|
| `aria2.max_concurrent_downloads` | 5 | Many simultaneous URLs/torrents | aria2 process memory grows |
| `aria2.max_connection_per_server` | 16 (upstream cap) | Single-source downloads slow | Remote servers may rate-limit |
| `aria2.split` | 16 (upstream cap) | Single-source downloads slow | Same as above |
| `aria2.disk_cache` | `"128M"` | Heavy parallel downloads thrashing disk | RSS grows by ~`disk_cache` |
| `telegram.upload_parallelism` | 4 | Large file splits to Telegram | Hitting per-channel API limits; CPU on TLS frames |
| `telegram.upload_chunk_size_kb` | 2048 | Saturating uplink to Telegram DC | TDLib clamps; very large chunks delay retransmit |
| `telegram.upload_files_parallelism` | 2 | Many small files uploaded to Telegram | More concurrent TDLib sends per chat |
| `google_drive.parallel_chunks_per_file` | 4 | Single-file uploads slow on high-RTT links | Memory grows by `parallel_chunks × chunk_size`; Google may 429 |
| `google_drive.parallel_files_per_directory` | 4 | Directory uploads dominate wall-time | Same as above; service-account quota burns faster |
| `google_drive.chunk_size` | `8388608` (8 MiB) | Want larger per-PUT payloads | Must remain 256 KiB-aligned; RAM = `parallel_chunks × chunk_size` per file |
| `rclone.transfers` | 8 | Many files in a single rclone command | RAM = `transfers × buffer_size`; remote may throttle |
| `rclone.checkers` | 16 | Slow listing on huge directories | API quota on the remote |
| `rclone.buffer_size` | `"32M"` | High-bandwidth remotes | RAM dominates here on constrained hosts |
| `rclone.multi_thread_streams` | 4 | Single-large-file uploads slow | Remote may rate-limit |

A rule of thumb: tune the external backend first. If a workload is bottlenecked,
the bottleneck is almost always network bandwidth or the downstream tool
(aria2, ffmpeg, rclone), not the bot's coroutine throughput. For RAM-constrained
hosts (<=1 GiB), `rclone.buffer_size` is the first knob to dial down because it
scales by `transfers`. For latency-bound destinations (Google Drive over a
high-RTT link), `parallel_chunks_per_file` is the first knob to dial up. See
`docs/throughput_benchmarks.md` for a per-scenario measurement template.

---

## Security checklist

Run through this on every deployment.

- [ ] `config.json` is **not** committed to git. Verify with `git check-ignore config.json`.
- [ ] `service_account.json` is **not** committed to git. Same check.
- [ ] `tdlib/` is **not** committed to git. It contains your encrypted session — if leaked, the bot account is compromised.
- [ ] File permissions on `config.json` are `0600` and owned by the service user. The same for `service_account.json` and the `tdlib/` directory (`0700` for the directory).
- [ ] `telegram.owner_id` is your real user id, not `0` or a placeholder. `0` fails validation and prevents startup.
- [ ] The service user is dedicated (`cmlb`), runs as non-root, and has a non-login shell.
- [ ] The systemd unit sets `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, and `ReadWritePaths=` only for the data, log, and downloads directories. The shipped unit in `packaging/systemd/cmlb.service` already does this — do not weaken it.
- [ ] Log files do not contain credentials. Search a sample:

      sudo grep -E '(api_hash|bot_token|service_account|ARIA2_SECRET|CMLB_ARIA2_SECRET)' /var/log/cmlb/cmlb.log

  Should return no matches. If anything appears, file a bug — CMLB redacts these in code; a leak means a regression.
- [ ] Backups are encrypted at rest. `cmlb.db` contains user ids, chat ids, and per-user settings; treat it as PII.
- [ ] Telegram bot token has been rotated since you first created the bot if it ever passed through a shared channel (chat message, screenshot, public CI log).
- [ ] Service-account JSON key has been rotated if it has ever been emailed or chat-shared.
- [ ] You have a tested restore procedure — not just a backup. A backup you've never restored is a hypothesis, not a backup.
