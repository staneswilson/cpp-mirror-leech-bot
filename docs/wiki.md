# GitHub Wiki

The repository wiki is intended as a short operator front door, not a second
copy of the full manual. The versioned documentation in this repository remains
the source of truth because it is reviewed, tested with the same pull request,
and tied to the exact commit being deployed.

Current state: GitHub reports the wiki feature as enabled, but the special wiki
git remote may not exist until the first page is created in the GitHub UI. If
`git ls-remote https://github.com/staneswilson/cpp-mirror-leech-bot.wiki.git`
returns `Repository not found`, create the first wiki page in GitHub, then push
the page set below.

---

## Publishing Workflow

```bash
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.wiki.git /tmp/cmlb-wiki
cd /tmp/cmlb-wiki
```

Create or update these files:

| Wiki page | Purpose |
|---|---|
| `Home.md` | Short landing page with links to the deployment path. |
| `_Sidebar.md` | Navigation for every wiki page. |
| `Quick-Docker-Deployment.md` | The shortest Docker path. |
| `BotFather-and-Telegram.md` | Telegram API app, bot token, owner ID, BotFather command menu. |
| `Configuration.md` | Required values and where they map in CMLB. |
| `Google-Drive.md` | Service account and Drive folder sharing. |
| `Operations.md` | Logs, stop, upgrade, backups, restore pointers. |

Commit and push:

```bash
git add .
git commit -S -m "docs: update CMLB wiki"
git push origin master
```

---

## `Home.md`

````markdown
# CMLB Wiki

CMLB is a C++23 Telegram mirror/leech bot that downloads through aria2 or
qBittorrent and uploads to Telegram, Google Drive, or rclone destinations.

Start here:

1. [Quick Docker Deployment](Quick-Docker-Deployment)
2. [BotFather and Telegram](BotFather-and-Telegram)
3. [Configuration](Configuration)
4. [Google Drive](Google-Drive)
5. [Operations](Operations)

The repository documentation is the source of truth:
https://github.com/staneswilson/cpp-mirror-leech-bot/tree/main/docs
````

---

## `_Sidebar.md`

````markdown
* [Home](Home)
* [Quick Docker Deployment](Quick-Docker-Deployment)
* [BotFather and Telegram](BotFather-and-Telegram)
* [Configuration](Configuration)
* [Google Drive](Google-Drive)
* [Operations](Operations)
````

---

## `Quick-Docker-Deployment.md`

````markdown
# Quick Docker Deployment

1. Install Docker and confirm `docker version` and `docker compose version`.
2. Clone CMLB and copy templates:

   ```bash
   git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
   cd cmlb
   cp config.example.json config.json
   cp packaging/.env.example packaging/.env
   sed -i "s/^ARIA2_SECRET=.*/ARIA2_SECRET=$(openssl rand -hex 32)/" packaging/.env
   ```

3. Fill `packaging/.env`:

   ```text
   TELEGRAM_API_ID=<from my.telegram.org/apps>
   TELEGRAM_API_HASH=<from my.telegram.org/apps>
   TELEGRAM_BOT_TOKEN=<from BotFather /newbot>
   OWNER_ID=<numeric id from @userinfobot>
   CMLB_TAG=sha-<full commit sha or release tag>
   ```

4. Start from a published image:

   ```bash
   docker compose -f packaging/docker-compose.yml --env-file packaging/.env config >/tmp/cmlb-compose.yml
   docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb aria2
   docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build
   docker compose -f packaging/docker-compose.yml logs -f cmlb
   ```

5. Verify in Telegram: `/start`, `/ping`, `/version`, `/status`.

Full guide: `docs/deployment_quickstart.md`.
````

---

## `BotFather-and-Telegram.md`

````markdown
# BotFather and Telegram

## Telegram API App

1. Open `https://my.telegram.org/apps`.
2. Create an app if needed.
3. Copy `App api_id` to `TELEGRAM_API_ID`.
4. Copy `App api_hash` to `TELEGRAM_API_HASH`.

## Bot Token

Talk to `@BotFather` and run `/newbot`. Copy the returned token to
`TELEGRAM_BOT_TOKEN`.

## Owner ID

Talk to `@userinfobot` and copy the numeric ID to `OWNER_ID`.

## Command Menu

In `@BotFather`, run `/setcommands`, choose the bot, and paste:

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
````

---

## `Configuration.md`

````markdown
# Configuration

CMLB loads `config.json`, applies explicit `CMLB_*` overrides, then validates
the resulting config.

Validate:

```bash
cmlb --validate-config /etc/cmlb/config.json
```

Minimum required values:

| Value | Source |
|---|---|
| `telegram.api_id` | `my.telegram.org/apps` |
| `telegram.api_hash` | `my.telegram.org/apps` |
| `telegram.bot_token` | BotFather `/newbot` |
| `telegram.owner_id` | `@userinfobot` |
| `aria2.secret` | `openssl rand -hex 32` |

Docker Compose reads `packaging/.env` and maps it to the `CMLB_*` variables.
Full reference: `docs/configuration_reference.md`.
````

---

## `Google-Drive.md`

````markdown
# Google Drive

1. Create or choose a Google Cloud project.
2. Enable the Google Drive API.
3. Create a service account.
4. Create and download a JSON key.
5. Save it as `service_account.json`.
6. Copy `client_email` from the JSON.
7. Share the destination Drive folder with that service-account email as Editor.
8. Set `google_drive.credentials_path` and `google_drive.parent_folder_id`.

Most Drive 403/404 deployment failures are folder-sharing mistakes.
Full guide: `docs/deployment_quickstart.md`.
````

---

## `Operations.md`

````markdown
# Operations

Docker logs:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env logs -f cmlb
```

Upgrade:

```bash
git pull --ff-only
nano packaging/.env
docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build
```

Back up `data/cmlb.db`, `tdlib/`, `config.json`, and `service_account.json`.
Full runbook: `docs/runbook.md`.
````
