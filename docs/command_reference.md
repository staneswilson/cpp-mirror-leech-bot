# CMLB command reference

User-facing reference for every `/command` CMLB exposes. Each entry includes syntax, the permission tier required, a description, example invocations, and common error modes.

For the permission model itself, see [`architecture.md`](architecture.md#domain). For the underlying use case implementations, see `application/`.

---

## Permission tiers

CMLB has four authority tiers, ordered from least to most privileged:

| Tier | Granted to | Notes |
|---|---|---|
| `Anyone` | every user the bot can see | `/start`, `/help`, `/ping`, `/settings`, `/version` |
| `User` | commands issued in `telegram.authorized_chats`; owner and sudo users also inherit this tier | `/mirror`, `/leech`, `/status`, `/stats` |
| `Admin` | users in `telegram.sudo_users` | Drive delete, bot settings, chat-wide bulk cancel |
| `Owner` | `telegram.owner_id` (single user) | `/log`, plus every lower-tier command |

A higher tier inherits everything from below.

---

## Table of contents

- [`/start`](#start)
- [`/help`](#help)
- [`/mirror`](#mirror)
- [`/leech`](#leech)
- [`/qbmirror`](#qbmirror)
- [`/qbleech`](#qbleech)
- [`/clone`](#clone)
- [`/count`](#count)
- [`/del`](#del)
- [`/status`](#status)
- [`/cancel`](#cancel)
- [`/cancelall`](#cancelall)
- [`/pause`](#pause)
- [`/resume`](#resume)
- [`/settings`](#settings)
- [`/botsettings`](#botsettings)
- [`/stats`](#stats)
- [`/ping`](#ping)
- [`/version`](#version)
- [`/log`](#log)
- [`/rss`](#rss)

---

## `/start`

**Syntax:** `/start`
**Permission:** Anyone

Greets the caller and confirms the bot is alive. Authorization is enforced on
the commands that require `User`, `Admin`, or `Owner`; `/start` itself remains
open so operators can verify that TDLib and the command dispatcher are alive.

**Examples:**

```
/start
```

**Errors:**

- None expected. If `/start` does not reply, the bot is not running or cannot reach Telegram.

---

## `/help`

**Syntax:** `/help`
**Permission:** Anyone

Shows the registered command list filtered to the caller's authority tier. The
current dispatcher ignores arguments to `/help`; per-command help lives in this
document.

**Examples:**

```
/help
```

**Errors:**

- None expected. Unknown command handling applies when the command itself is
  unknown, not to `/help` arguments.

---

## `/mirror`

**Syntax:** `/mirror <url-or-magnet>`
**Permission:** User

Downloads the resource at the given URL or magnet link and uploads the result
to the caller's configured mirror destination. The download uses aria2 by
default; for torrent-specific qBittorrent behaviour use [`/qbmirror`](#qbmirror).
Destination selection is controlled by `/settings` and persisted in SQLite.
Shortcut: `/m`.

The bot replies immediately with a queued message and edits it with live progress. On completion the message changes to a link to the uploaded resource.

**Examples:**

```
/mirror https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso
/mirror magnet:?xt=urn:btih:...
```

**Errors:**

- `Input invalid: not a URL or magnet` — argument couldn't be parsed.
- `Auth forbidden` — caller does not have the `User` tier.
- `External unavailable: aria2` — aria2c is not reachable; see [`runbook.md`](runbook.md#aria2c-not-reachable).
- `External quota exceeded: gdrive` — service account has hit its quota or the destination folder is not writable.
- `Io full` — the host has run out of disk space in `paths.download_dir`.

---

## `/leech`

**Syntax:** `/leech <url-or-magnet>`
**Permission:** User

Same as `/mirror`, but the result is uploaded back into the originating
Telegram chat. Files larger than the bot-wide `leech_split_size` setting are
split before upload. Per-user settings decide whether Telegram uploads are sent
as documents and whether a default thumbnail path is attached.
Shortcut: `/l`.

**Examples:**

```
/leech https://example.com/big.iso
/leech magnet:?xt=urn:btih:...
```

**Errors:**

- Same set as `/mirror`, plus:
- `External rate limited: telegram` — Telegram's per-chat upload rate limit was hit. CMLB will retry with backoff; persistent rate-limiting may indicate the chat or bot is flooded.
- `Input too large` — file exceeds the configured split policy (the bot refuses unbounded splits).

---

## `/qbmirror`

**Syntax:** `/qbmirror <url-or-magnet>`
**Permission:** User

Same semantics as `/mirror`, but forces the qBittorrent downloader. Use it for
magnet links or torrent URLs when you want qBittorrent's DHT, tracker, or
seeding behaviour. The current command dispatcher expects the input as command
text; Telegram file-reply torrent ingestion is not wired yet.
Shortcut: `/qm`.

**Examples:**

```
/qbmirror magnet:?xt=urn:btih:...
```

**Errors:**

- `External unavailable: qbittorrent` — Web API not reachable.
- `External unauthorized: qbittorrent` — credentials wrong.
- `Input invalid` — no usable URL or magnet text was supplied.

---

## `/qbleech`

**Syntax:** `/qbleech <url-or-magnet>`
**Permission:** User

The qBittorrent equivalent of `/leech`. See `/qbmirror` and `/leech` for details.
Shortcut: `/ql`.

**Examples:**

```
/qbleech magnet:?xt=urn:btih:...
```

**Errors:** combined set of `/leech` and `/qbmirror`.

---

## `/clone`

**Syntax:** `/clone <gdrive-link>`
**Permission:** User

Server-side clone of a Google Drive resource (file or folder) into the configured destination. No bytes are downloaded to CMLB's host; Drive copies the file to the destination owned by the service account. Faster and quota-free for downloads.

The destination is `google_drive.parent_folder_id`. On success the reply is a link to the cloned resource.

**Examples:**

```
/clone https://drive.google.com/drive/folders/0Abc...
/clone https://drive.google.com/file/d/1Xyz.../view
```

**Errors:**

- `Input invalid` — argument is not a recognizable Drive URL.
- `External unauthorized: gdrive` — service account cannot see the source. Share the source folder with the service account email.
- `External quota exceeded: gdrive` — daily user copy quota hit (Drive limits server-side copies). Retry after 24 hours.
- `State conflict` — destination already contains a folder with the same name. Rename or pre-clean.

---

## `/count`

**Syntax:** `/count <gdrive-link>`
**Permission:** User

Recursively counts files, subfolders, and total bytes under the given Drive resource. Read-only.

**Examples:**

```
/count https://drive.google.com/drive/folders/0Abc...
```

**Errors:**

- Same as `/clone` minus the destination-related ones.

---

## `/del`

**Syntax:** `/del <gdrive-link>`
**Permission:** Admin

Deletes a Drive resource owned by, or trashable by, the service account. Because
this is destructive the command requires `Admin`. The current implementation
executes immediately after the command is authorized; do not hand out `Admin`
unless that is acceptable operationally.

**Examples:**

```
/del https://drive.google.com/drive/folders/0Abc...
```

**Errors:**

- `Auth forbidden` — caller is not `Admin`.
- `External unauthorized: gdrive` — service account does not own and cannot trash the resource.
- `State not found` — already deleted or wrong link.

---

## `/status`

**Syntax:** `/status [<task-id>]`
**Permission:** User

Without an argument: sends a point-in-time status summary for active tasks the
caller can see. `User` sees their own tasks in the current chat; `Admin` and
`Owner` see all tasks in that chat.

With an argument: shows one visible task by id, including progress, rate, ETA,
state, host metrics, and the current downloader state when the downloader
binding is still available.

**Examples:**

```
/status
/status t-42
```

**Errors:**

- `State not found` — task id does not exist or was completed and reaped.

---

## `/cancel`

**Syntax:** `/cancel <task-id>`
**Permission:** User

Cancels a non-terminal task in the current chat. If the task is actively running,
the live use case observes the cancellation flag, asks the downloader to remove
the job when possible, transitions the task to `Cancelled`, and edits the status
message.

**Examples:**

```
/cancel t-42
```

**Errors:**

- `Auth forbidden` — caller cannot see this chat/task.
- `State not found` — task id does not exist.
- `State invalid transition` — task already completed; nothing to cancel.

---

## `/cancelall`

**Syntax:** `/cancelall`
**Permission:** Admin

Cancels every active task in the current chat. This is intentionally admin-only because it is destructive for everyone in that chat.

**Examples:**

```
/cancelall
```

**Errors:**

- None typically. If there are no active tasks, the summary reports
  `0 cancelled, 0 failed`.

---

## `/pause`

**Syntax:** `/pause <task-id>`
**Permission:** User

Pauses a non-terminal task in the current chat if the underlying downloader
supports it. Pause/resume is forwarded to the downloader; CMLB does not persist
a separate `Paused` task state in SQLite.

**Examples:**

```
/pause t-42
```

**Errors:**

- `State invalid transition` — task is not in a pausable state, or the downloader does not support pause.

---

## `/resume`

**Syntax:** `/resume <task-id>`
**Permission:** User

Resumes a non-terminal task in the current chat by forwarding the request to the
downloader.

**Examples:**

```
/resume t-42
```

**Errors:**

- `State invalid transition` — task was not paused.

---

## `/settings`

**Syntax:** `/settings`
**Permission:** Anyone

Opens an inline-keyboard panel for the caller's per-user settings. Current buttons include:

- Default upload destination (Drive / rclone remote / Telegram).
- Upload-as-document mode.

Settings are persisted in the SQLite database and survive restarts.

**Examples:**

```
/settings
```

**Errors:**

- None typically. If a keyboard button fails (e.g. typing a new rename pattern times out) the panel will reopen unchanged.

---

## `/botsettings`

**Syntax:** `/botsettings`
**Permission:** Admin

Shows the bot-wide settings panel. The current panel is read-only except for the close button; runtime mutation should be added deliberately through `UpdateBotSettings`.

- Default upload destination.
- Download directory.
- Telegram leech split size.

Process-level settings such as `telegram.bot_token`, database path, and logger directory are config-file settings and require a restart.

**Examples:**

```
/botsettings
```

**Errors:**

- `Auth forbidden` — caller is not an admin or owner.

---

## `/stats`

**Syntax:** `/stats`
**Permission:** User

Shows system statistics:

- CPU load (1 / 5 / 15 minute averages on Linux).
- Memory usage (used / total).
- Disk usage for the bot process working directory.
- Bot uptime.
- Active downloader count aggregated from aria2 and qBittorrent.
- Any downloader backend whose aggregate stats are temporarily unavailable.

**Examples:**

```
/stats
```

**Errors:** none expected.

---

## `/ping`

**Syntax:** `/ping`
**Permission:** Anyone

Replies with the round-trip latency between the bot receiving the message and the reply being delivered to Telegram. Useful as a sanity check.

**Examples:**

```
/ping
```

**Errors:** none. A missing reply means the bot is dead or disconnected.

---

## `/version`

**Syntax:** `/version`
**Permission:** Anyone

Shows the running CMLB build version.

**Examples:**

```
/version
```

**Errors:** none expected.

---

## `/log`

**Syntax:** `/log`
**Permission:** Owner

Uploads `logs/cmlb.log` as a document, capped at 50 MiB so an operator cannot accidentally push a huge log segment into Telegram.

**Examples:**

```
/log
```

**Errors:**

- `Auth forbidden` — caller is not the owner.
- `Io not found` — `logs/cmlb.log` does not exist in the process working directory.

---

## `/rss`

`/rss` is a multi-subcommand surface for managing RSS subscriptions. RSS
polling runs as part of the main process; there is no separate `rss.enabled`
config switch in v1.

### `/rss add`

**Syntax:** `/rss add <url>`
**Permission:** User

Creates a subscription for the current chat. The URL must start with `http://` or `https://`. Polling behavior is owned by `RssFeedPoller`.

**Examples:**

```
/rss add https://example.com/feed.xml
```

**Errors:**

- `Input invalid: URL` — could not parse.

### `/rss list`

**Syntax:** `/rss list`
**Permission:** User

Lists subscriptions for the current chat.

**Examples:**

```
/rss list
```

**Errors:** none.

### `/rss remove`

**Syntax:** `/rss remove <id>`
**Permission:** User

Removes a subscription by numeric id. Users can remove feeds owned by the
current chat. `/rss delete <id>` and `/rss del <id>` are accepted aliases.

**Examples:**

```
/rss remove 42
```

**Errors:**

- `State not found` — no visible subscription has that id.
- `Auth forbidden` — feed belongs to another chat.
