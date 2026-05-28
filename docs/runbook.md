# CMLB operator runbook

This document is for the person who runs CMLB in production. It assumes you are comfortable on a Linux shell, with systemd, and with reading log files. If you are looking for how to *develop* CMLB, read [`CONTRIBUTING.md`](../CONTRIBUTING.md). If you are looking for what each config field means, read [`configuration_reference.md`](configuration_reference.md).

---

## Table of contents

- [Prerequisites](#prerequisites)
- [Initial setup](#initial-setup)
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

- Linux x86_64 — primary target. Tested on Ubuntu 22.04 LTS, Debian 12, Fedora 39.
- Linux aarch64 — supported, built in CI, but binary not currently published. Build from source.
- Windows — supported for development. Production deployments are not recommended (TDLib session locking, filesystem case-sensitivity edge cases).
- macOS — supported for development. Not a production target.

**Toolchain (build from source only):**

- A C++23 compiler: GCC 13+, Clang 17+, MSVC 19.38+ (Visual Studio 2022 17.8+), or Apple Clang 15+.
- CMake 3.28 or newer.
- Ninja (the default generator).
- `git`, `curl`, `zip`, `unzip`, `pkg-config`, `gperf`.
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

```bash
# 1. Create a service user (recommended for systemd installs)
sudo useradd -r -s /usr/sbin/nologin -d /var/lib/cmlb -m cmlb

# 2. Clone the repository
sudo -u cmlb git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git /var/lib/cmlb/src
cd /var/lib/cmlb/src

# 3. Install vcpkg (manifest mode; do not modify vcpkg.json by hand)
sudo -u cmlb git clone https://github.com/microsoft/vcpkg.git /var/lib/cmlb/vcpkg
sudo -u cmlb /var/lib/cmlb/vcpkg/bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT=/var/lib/cmlb/vcpkg' | sudo tee /etc/profile.d/cmlb.sh

# 4. Build the release preset
sudo -u cmlb -E VCPKG_ROOT=/var/lib/cmlb/vcpkg \
    cmake --preset release
sudo -u cmlb -E VCPKG_ROOT=/var/lib/cmlb/vcpkg \
    cmake --build --preset release -j

# 5. Install the binary system-wide (or run from the build tree)
sudo install -m 0755 build/release/cmlb /usr/local/bin/cmlb

# 6. Verify the binary
/usr/local/bin/cmlb --version
```

The Docker path is shorter:

```bash
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb
cp config.example.json config.json   # edit before bringing up
docker compose up -d
```

If you use the systemd installer (`scripts/install.sh`) it performs steps 1, 5, and creates `/etc/systemd/system/cmlb.service` from the template in `packaging/systemd/`.

---

## Configuration

CMLB reads its configuration from `config.json`. The full schema is in [`configuration_reference.md`](configuration_reference.md). This section covers only what an operator must obtain before first run.

### Obtaining Telegram credentials

You need two pieces of identity:

1. **`telegram.api_id`** and **`telegram.api_hash`** — these identify *your application* to Telegram, not your bot. Get them from <https://my.telegram.org/apps>. Sign in with the phone number that will own the application, fill the form (any name is fine), and copy the `App api_id` and `App api_hash`. **Keep `api_hash` secret.**

2. **`telegram.bot_token`** — this identifies the bot account. Open Telegram, talk to [@BotFather](https://t.me/BotFather), send `/newbot`, follow the prompts. BotFather replies with a token of the form `123456789:AAH...`. Paste it into `config.json`. **Keep the bot token secret** — anyone with it can impersonate the bot.

3. **`telegram.owner_id`** — your numeric Telegram user id. Talk to [@userinfobot](https://t.me/userinfobot) to retrieve it. The owner has unrestricted access including `/log` and `/botsettings`.

> **Warning:** If you run CMLB as a *user account* (not a bot) the auth flow differs: you provide a phone number and CMLB will prompt for the SMS code on first start. Bots are the recommended deployment mode; user-account mode is for niche cases (channels with bot restrictions, larger upload quotas pre-premium).

### Obtaining a Google service account (optional)

Only required if you use Google Drive for upload, clone, count, or delete.

1. In Google Cloud Console, create a project.
2. Enable the Drive API on that project.
3. Create a *service account*, give it a name, no roles.
4. Generate a JSON key for the service account; download it.
5. Save the file as `service_account.json` next to `config.json` (or wherever `google_drive.service_account_path` points).
6. **Share the destination Drive folder with the service account email** (`<name>@<project>.iam.gserviceaccount.com`) as Editor. This is the step that's easy to forget and produces 404s.

### Validating the config

Before starting the bot, validate the configuration:

```bash
cmlb --config /etc/cmlb/config.json --validate-config
```

This loads the file, applies env overrides, and runs the validator. A non-zero exit code means the config is broken; the report lists every problem at once.

---

## First run

```bash
cmlb --config /etc/cmlb/config.json
```

What happens on the first start:

1. **Schema migration.** CMLB checks `data/cmlb.db` for the `schema_version` row. If missing or behind, every migration above the current version is applied in order. The first start applies all of them.
2. **TDLib authentication.** TDLib creates `tdlib/` and goes through the auth handshake. For bots this is one call (`checkAuthenticationBotToken`); for user accounts the bot will prompt on stdin for the verification code Telegram sends to your phone (and, if 2FA is enabled, the password). After successful auth, the `tdlib/` directory holds the encrypted session — keep it.
3. **Adapter startup.** Aria2, qBittorrent, and Google Drive adapters connect lazily on first use. Failures are logged but do not block startup.
4. **Update loop.** CMLB subscribes to TDLib updates and begins processing.

You should see, within a few seconds:

```
[info] cmlb 0.1.0-alpha starting
[info] config loaded from /etc/cmlb/config.json
[info] schema at version 2 (latest)
[info] telegram authenticated as @your_bot (id 123...)
[info] ready
```

Send `/start` to your bot to confirm end-to-end. If you are the configured owner, you should get a "Welcome, owner" reply.

---

## Updating

```bash
# As the service user, in the source tree:
git fetch origin
git checkout v0.2.0      # or the tag you want
git submodule update --init --recursive  # only if the project pulled in submodules

# Rebuild
cmake --preset release
cmake --build --preset release

# Apply migrations only (does not start the service)
sudo systemctl stop cmlb
sudo -u cmlb /usr/local/bin/cmlb --config /etc/cmlb/config.json --migrate-only

# Reinstall the binary and start
sudo install -m 0755 build/release/cmlb /usr/local/bin/cmlb
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
| `data/cmlb.log` and rotated logs | Log history | At your discretion |

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
sudo -u cmlb /usr/local/bin/cmlb --config /etc/cmlb/config.json --validate-config

# Run migrations forward if the backup is older than the current binary
sudo -u cmlb /usr/local/bin/cmlb --config /etc/cmlb/config.json --migrate-only

sudo systemctl start cmlb
sudo journalctl -u cmlb -f
```

If the TDLib session was not restored, the first start will re-prompt for authentication.

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
5. Validate: `cmlb --validate-config`.
6. Start: `sudo systemctl start cmlb`. The bot will re-authenticate with the new token automatically.

**Google service account key:**

1. In Google Cloud Console, create a new JSON key for the same service account (or a new account if you want to retire the old principal entirely).
2. Place the new file at `google_drive.service_account_path`.
3. Restart the service.
4. In Cloud Console, delete the old key.

**aria2 RPC secret / qBittorrent password:**

Rotate the credential in the downstream tool first, then update the matching field in `config.json`, then restart CMLB.

### Graceful shutdown / draining

CMLB handles `SIGTERM` and `SIGINT` as drain signals:

1. Stops accepting new updates (the TDLib loop is paused).
2. Marks any in-progress task as `Cancelled` *unless* it is in a stage that can be safely resumed (aria2 saves its session, qBittorrent retains state, partial downloads stay on disk).
3. Flushes the log sink.
4. Closes the SQLite connection pool (which checkpoints WAL).
5. Exits 0.

```bash
sudo systemctl stop cmlb         # sends SIGTERM, waits, then SIGKILL after timeout
```

Default systemd `TimeoutStopSec` in the shipped unit is 30 seconds. Increase it if your typical task drain takes longer:

```bash
sudo systemctl edit cmlb
# under [Service]:
# TimeoutStopSec=120
```

`SIGKILL` is brutally fine: SQLite WAL means the database survives, in-flight downloads remain on disk, the TDLib session is intact. The only cost is that paused tasks need manual resume.

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

4. Verify the secret matches `aria2.rpc_secret` in CMLB's config. A mismatch surfaces as `auth: unauthorized` in CMLB logs, not as a connection failure.
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

1. Confirm `google_drive.service_account_path` points at a real, parseable JSON key.
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
| `executor.worker_threads` | `max(4, 2 × hardware_concurrency)` | Many small concurrent tasks | Diminishing returns past hw concurrency; memory grows |
| `aria2.max_concurrent_downloads` | 16 | Many simultaneous URLs/torrents | aria2 process memory grows |
| `aria2.max_connection_per_server` | 16 (upstream cap) | Single-source downloads slow | Remote servers may rate-limit |
| `aria2.split` | 16 (upstream cap) | Single-source downloads slow | Same as above |
| `aria2.disk_cache` | `"128M"` | Heavy parallel downloads thrashing disk | RSS grows by ~`disk_cache` |
| `telegram.upload_parallelism` | 4 | Large file splits to Telegram | Hitting per-channel API limits; CPU on TLS frames |
| `telegram.upload_chunk_size_kb` | 2048 | Saturating uplink to Telegram DC | TDLib clamps; very large chunks delay retransmit |
| `telegram.upload_split_bytes` | `2_000_000_000` (2 GB) | Bot has Telegram Premium | Set to `4_000_000_000`; otherwise sends will fail |
| `telegram.progress_edit_interval_ms` | 2000 | Faster status feedback | Hitting Telegram rate limit (1 edit/sec/chat is the cap) |
| `google_drive.parallel_chunks_per_file` | 4 | Single-file uploads slow on high-RTT links | Memory grows by `parallel_chunks × chunk_size`; Google may 429 |
| `google_drive.parallel_files_per_directory` | 4 | Directory uploads dominate wall-time | Same as above; service-account quota burns faster |
| `google_drive.chunk_size` | `8388608` (8 MiB) | Want larger per-PUT payloads | Must remain 256 KiB-aligned; RAM = `parallel_chunks × chunk_size` per file |
| `rclone.transfers` | 8 | Many files in a single rclone command | RAM = `transfers × buffer_size`; remote may throttle |
| `rclone.checkers` | 16 | Slow listing on huge directories | API quota on the remote |
| `rclone.buffer_size` | `"32M"` | High-bandwidth remotes | RAM dominates here on constrained hosts |
| `rclone.multi_thread_streams` | 4 | Single-large-file uploads slow | Remote may rate-limit |
| `database.connection_pool_size` | 8 | Heavy concurrent reads | Diminishing returns past 8 with WAL |
| `metrics.enabled` | `false` | Want Prometheus visibility | Negligible cost |

A rule of thumb: increase `executor.worker_threads` last. If a workload is bottlenecked, the bottleneck is almost always network bandwidth or the downstream tool (aria2, ffmpeg, rclone), not the bot's coroutine throughput. For RAM-constrained hosts (≤1 GiB), `rclone.buffer_size` is the first knob to dial down — it scales by `transfers`. For latency-bound destinations (Google Drive over a high-RTT link), `parallel_chunks_per_file` is the first knob to dial up. See `docs/throughput_benchmarks.md` for a per-scenario measurement template and the exact theoretical-ceiling formulas.

---

## Security checklist

Run through this on every deployment.

- [ ] `config.json` is **not** committed to git. Verify with `git check-ignore config.json`.
- [ ] `service_account.json` is **not** committed to git. Same check.
- [ ] `tdlib/` is **not** committed to git. It contains your encrypted session — if leaked, the bot account is compromised.
- [ ] File permissions on `config.json` are `0600` and owned by the service user. The same for `service_account.json` and the `tdlib/` directory (`0700` for the directory).
- [ ] `telegram.owner_id` is your real user id, not `0` or a placeholder. The owner bypasses all permission checks; an open `0` makes the bot world-administered.
- [ ] The service user is dedicated (`cmlb`), runs as non-root, and has a non-login shell.
- [ ] The systemd unit sets `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, and `ReadWritePaths=` only for the data, log, and downloads directories. The shipped unit in `packaging/systemd/cmlb.service` already does this — do not weaken it.
- [ ] Metrics endpoint (`metrics.bind`) is bound to `127.0.0.1` unless you have an authenticating reverse proxy in front of it.
- [ ] Log files do not contain credentials. Search a sample:

      sudo grep -E '(api_hash|bot_token|service_account|rpc_secret)' /var/lib/cmlb/data/cmlb.log

  Should return no matches. If anything appears, file a bug — CMLB redacts these in code; a leak means a regression.
- [ ] Backups are encrypted at rest. `cmlb.db` contains user ids, chat ids, and per-user settings; treat it as PII.
- [ ] Telegram bot token has been rotated since you first created the bot if it ever passed through a shared channel (chat message, screenshot, public CI log).
- [ ] Service-account JSON key has been rotated if it has ever been emailed or chat-shared.
- [ ] You have a tested restore procedure — not just a backup. A backup you've never restored is a hypothesis, not a backup.
