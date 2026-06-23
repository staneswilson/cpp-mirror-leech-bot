# CMLB command reference

User-facing reference for every `/command` CMLB exposes. Each entry includes syntax, the permission tier required, a description, example invocations, and common error modes.

For the permission model itself, see [`architecture.md`](architecture.md#domain). For the underlying use case implementations, see `application/`.

---

## Permission tiers

CMLB has four authority tiers, ordered from least to most privileged:

| Tier | Granted to | Notes |
|---|---|---|
| `Anyone` | every user the bot can see | `/start`, `/help`, `/ping` |
| `User` | users in `telegram.users`, or anyone if the list is empty | `/mirror`, `/leech`, `/status`, `/settings`, `/stats` |
| `Admin` | users in `telegram.admins` | per-user overrides, force-cancel others' tasks |
| `Owner` | `telegram.owner_id` (single user) | `/log`, `/botsettings`, bypass all limits |

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
- [`/log`](#log)
- [`/rss`](#rss)

---

## `/start`

**Syntax:** `/start`
**Permission:** Anyone

Greets the caller and confirms the bot is alive. If the caller is not in any allow list and the bot is configured with a non-empty `telegram.users` list, `/start` will inform them they are unauthorized — this is by design, so an operator can see who is trying to use the bot without granting any access.

**Examples:**

```
/start
```

**Errors:**

- None expected. If `/start` does not reply, the bot is not running or cannot reach Telegram.

---

## `/help`

**Syntax:** `/help [<command>]`
**Permission:** Anyone

Without an argument, shows a grouped command list filtered to the caller's authority tier. With an argument, shows the per-command help (syntax, description, examples) for that command. `/help mirror` shows the same body that appears here under [`/mirror`](#mirror).

**Examples:**

```
/help
/help mirror
/help rss
```

**Errors:**

- `Unknown command` — argument did not match any registered command.

---

## `/mirror`

**Syntax:** `/mirror <url-or-magnet> [--no-extract] [--upload-to <destination>] [--rename <name>]`
**Permission:** User

Downloads the resource at the given URL or magnet link and uploads the result to the configured cloud destination (Google Drive or rclone remote). The download uses aria2 by default; for torrent-specific behaviour use [`/qbmirror`](#qbmirror).

Flags:

- `--no-extract` — do not auto-extract archives after download. Default behaviour extracts `.7z`, `.zip`, `.tar*`, `.rar`, and uploads the extracted contents.
- `--upload-to <destination>` — override the configured default destination. Valid values: `gdrive`, `rclone:<remote>:<path>`, `tg` (same as `/leech`).
- `--rename <name>` — rename the resulting file or top-level folder before upload.

The bot replies immediately with a queued message and edits it with live progress. On completion the message changes to a link to the uploaded resource.

**Examples:**

```
/mirror https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso
/mirror https://example.com/archive.tar.gz --no-extract
/mirror magnet:?xt=urn:btih:... --upload-to rclone:backup:isos
```

**Errors:**

- `Input invalid: not a URL or magnet` — argument couldn't be parsed.
- `Auth forbidden` — caller does not have the `User` tier.
- `External unavailable: aria2` — aria2c is not reachable; see [`runbook.md`](runbook.md#aria2c-not-reachable).
- `External quota exceeded: gdrive` — service account has hit its quota or the destination folder is not writable.
- `Io full` — the host has run out of disk space in `paths.downloads`.

---

## `/leech`

**Syntax:** `/leech <url-or-magnet> [--no-extract] [--rename <name>] [--thumbnail <reply>]`
**Permission:** User

Same as `/mirror`, but the result is uploaded back into the originating Telegram chat as a document (or video, when applicable) rather than to a cloud destination. Files larger than `telegram.upload_split_bytes` are split.

Flags:

- `--no-extract` — same as for `/mirror`.
- `--rename <name>` — same as for `/mirror`.
- `--thumbnail <reply>` — if used in reply to a photo, attaches that photo as the document thumbnail.

**Examples:**

```
/leech https://example.com/big.iso
/leech magnet:?xt=urn:btih:... --rename "MyShow S01E01"
```

(reply to a photo)

```
/leech https://example.com/movie.mkv --thumbnail
```

**Errors:**

- Same set as `/mirror`, plus:
- `External rate limited: telegram` — Telegram's per-chat upload rate limit was hit. CMLB will retry with backoff; persistent rate-limiting may indicate the chat or bot is flooded.
- `Input too large` — file exceeds `telegram.upload_split_bytes * 50` (the bot refuses unbounded splits).

---

## `/qbmirror`

**Syntax:** `/qbmirror <url-or-magnet-or-torrent-file> [--no-extract] [--upload-to <destination>]`
**Permission:** User

Same semantics as `/mirror`, but forces the qBittorrent downloader. Use when:

- The source is a `.torrent` file attachment (reply to a file with `/qbmirror`).
- You want qBittorrent's DHT or specific tracker behaviour.
- aria2 is misbehaving on this specific torrent.

Requires `qbittorrent.enabled = true`.

**Examples:**

```
/qbmirror magnet:?xt=urn:btih:...
```

(reply to a .torrent attachment)

```
/qbmirror
```

**Errors:**

- `External unavailable: qbittorrent` — Web API not reachable.
- `External unauthorized: qbittorrent` — credentials wrong.
- `Input invalid` — no usable input. If replying to a non-torrent file you'll get this.

---

## `/qbleech`

**Syntax:** `/qbleech <url-or-magnet-or-torrent-file> [--no-extract]`
**Permission:** User

The qBittorrent equivalent of `/leech`. See `/qbmirror` and `/leech` for details.

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

The destination is `google_drive.default_folder_id` (or `google_drive.shared_drive_id` if set). On success the reply is a link to the cloned resource.

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

Permanently deletes a Drive resource owned by the service account (or trashable by it). Because this is destructive the command requires `Admin`. The bot replies with a confirmation inline keyboard before performing the delete.

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

Without an argument: shows all active tasks the caller can see. `User` sees their own tasks; `Admin` and `Owner` see everyone's. The status message updates in place on a throttle (default once every 2 seconds).

With an argument: shows the detailed status of a single task by id (the short id returned when the task was created), including stage, progress, speed, ETA, and the last 3 log lines for that task.

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
**Permission:** User (callers can cancel their own tasks) / Admin (cancels anyone's)

Cancels the task. Triggers the task's cancellation slot; any in-flight downloader/uploader call returns `Cancelled`. The task transitions to the `Cancelled` state, the progress message is edited to `"Cancelled."`, and partial files on disk are removed (unless `paths.keep_downloads_on_failure = true`).

**Examples:**

```
/cancel t-42
```

**Errors:**

- `Auth forbidden` — `User` tier and not the task owner.
- `State not found` — task id does not exist.
- `State invalid transition` — task already completed; nothing to cancel.

---

## `/cancelall`

**Syntax:** `/cancelall`
**Permission:** User (cancels own tasks) / Admin (cancels everyone's)

Cancels every active task visible to the caller. For `User`, that's their own. For `Admin` and `Owner`, that's the bot-wide active set.

**Examples:**

```
/cancelall
```

**Errors:**

- None typically. If there are no tasks the bot replies `Nothing to cancel.`

---

## `/pause`

**Syntax:** `/pause <task-id>`
**Permission:** User (own tasks) / Admin (any)

Pauses the task if the underlying downloader supports it (aria2 and qBittorrent both do for torrents; HTTP/FTP downloads cannot be paused, only cancelled). Paused tasks consume no network and do not progress, but they hold their downloaded bytes.

**Examples:**

```
/pause t-42
```

**Errors:**

- `State invalid transition` — task is not in a pausable state, or the downloader does not support pause.

---

## `/resume`

**Syntax:** `/resume <task-id>`
**Permission:** User (own tasks) / Admin (any)

Resumes a paused task.

**Examples:**

```
/resume t-42
```

**Errors:**

- `State invalid transition` — task was not paused.

---

## `/settings`

**Syntax:** `/settings`
**Permission:** User

Opens an inline-keyboard panel for the caller's per-user settings. Toggles include:

- Default upload destination (Drive / rclone remote / Telegram).
- Auto-extract on / off.
- Default rename pattern.
- Per-user log verbosity for status messages.

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
**Permission:** Owner

Opens an inline-keyboard panel for bot-wide settings that can be edited at runtime (a subset of `config.json` — fields marked `runtime-editable` in [`configuration_reference.md`](configuration_reference.md)). Includes:

- Default upload destination.
- Global limits (`max_concurrent_tasks_global`, `max_filesize_bytes`).
- RSS feature on/off.
- Logging level (process-wide).

> **Note:** Some settings (e.g. `telegram.bot_token`, database path) cannot be changed at runtime and must be edited in `config.json` followed by a restart.

**Examples:**

```
/botsettings
```

**Errors:**

- `Auth forbidden` — caller is not the owner.

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

## `/log`

**Syntax:** `/log [<lines>] [--level <level>] [--module <name>]`
**Permission:** Owner

Tails the last N log lines (default 50, max 500) from the bot's log file. Optionally filters by level (`info`, `warn`, `error`, `debug`) or by module name (e.g. `aria2`, `gdrive`, `tdlib`).

The output is sent as a document attachment for any request longer than ~3500 characters.

**Examples:**

```
/log
/log 200
/log 100 --level error
/log 50 --module aria2
```

**Errors:**

- `Auth forbidden` — caller is not the owner.
- `Io not found` — log file does not exist or `logging.file` is empty.

---

## `/rss`

`/rss` is a multi-subcommand surface for managing RSS subscriptions. Requires `rss.enabled = true`.

### `/rss add`

**Syntax:** `/rss add <name> <url> [--interval <seconds>] [--filter <regex>] [--chat <chat-id>]`
**Permission:** User

Creates a new subscription named `<name>` polling `<url>`. The first poll happens immediately; subsequent polls at the configured interval (default `rss.default_interval_seconds`).

Flags:

- `--interval <seconds>` — override the poll interval. Cannot be lower than `rss.min_interval_seconds`.
- `--filter <regex>` — only accept entries whose title matches this regex (ECMAScript syntax).
- `--chat <chat-id>` — deliver matches to a specific chat. Defaults to the chat the command was issued in.

When a feed produces a new entry that matches the filter, the bot announces it in the destination chat. The user must then trigger `/mirror` or `/leech` on the link if they want to download it. (Automatic auto-mirror-on-match is a planned v2 feature.)

**Examples:**

```
/rss add ubuntu https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso.zsync
/rss add ubuntu https://example.com/feed.xml --interval 1800 --filter "^Ubuntu .* LTS"
```

**Errors:**

- `Input invalid: name already exists` — pick a different name.
- `Input invalid: URL` — could not parse.
- `Input invalid: regex` — filter is not a valid regex.
- `External unavailable` — the first probe of the feed failed.

### `/rss list`

**Syntax:** `/rss list`
**Permission:** User

Lists the caller's subscriptions with their URL, interval, filter, last poll time, and last delivered entry. Admins see all subscriptions.

**Examples:**

```
/rss list
```

**Errors:** none.

### `/rss remove`

**Syntax:** `/rss remove <name>`
**Permission:** User (own) / Admin (any)

Removes the named subscription. The poller stops immediately; any in-flight poll for that feed is cancelled.

**Examples:**

```
/rss remove ubuntu
```

**Errors:**

- `State not found` — no subscription with that name (or visible to caller).
- `Auth forbidden` — `User` tier trying to remove someone else's subscription.
