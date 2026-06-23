# CMLB Deployment Quickstart

This guide starts from an empty Linux machine and ends with a running bot. It
uses Docker Compose because that is the simplest production-shaped deployment:
CMLB runs in one container, aria2 runs in another container, and persistent
state lives in Docker volumes.

Use this path first. Build-from-source, systemd, backups, and performance
tuning live in [`runbook.md`](runbook.md).

---

## Deployment Flow

Follow the guide in this order:

1. Install Docker.
2. Create Telegram credentials.
3. Register the bot commands in BotFather.
4. Fill the deployment worksheet.
5. Copy the example files and set `packaging/.env`.
6. Start CMLB and verify the bot replies.
7. Add optional services: Google Drive, rclone, or qBittorrent.

The core Telegram + aria2 deployment works without Google Drive, rclone, or
qBittorrent. Add those services only when you need their commands.

---

## What You Will Create

| Item | Why CMLB needs it |
|---|---|
| Telegram API app | TDLib requires `api_id` and `api_hash` for every client. |
| Telegram bot | The bot account users talk to in Telegram. |
| BotFather commands | The command menu shown inside Telegram clients. |
| Owner user ID | Grants you Owner permission inside CMLB. |
| aria2 RPC secret | Protects the downloader RPC endpoint. |
| Pinned CMLB image tag | Makes deployment reproducible. |

Optional:

| Optional service | Needed for |
|---|---|
| Google Drive service account | `/mirror`, `/clone`, `/count`, `/del` with Drive. |
| rclone remote | `/mirror` to an rclone destination. |
| qBittorrent Web UI | `/qbmirror` and `/qbleech`. |

---

## Deployment Worksheet

Fill this worksheet before editing files. It prevents the common mistake of
starting containers with placeholders still in the environment.

| Name | Example | Your value |
|---|---|---|
| `TELEGRAM_API_ID` | `123456` |  |
| `TELEGRAM_API_HASH` | `0123456789abcdef0123456789abcdef` |  |
| `TELEGRAM_BOT_TOKEN` | `1234567890:AAExampleTokenValue` |  |
| `OWNER_ID` | `987654321` |  |
| `ARIA2_SECRET` | `openssl rand -hex 32` output |  |
| `CMLB_TAG` | `sha-<full commit>` or `vX.Y.Z` |  |
| Google Drive folder ID | `1AbCd...` | optional |
| Google service-account email | `cmlb@project.iam.gserviceaccount.com` | optional |
| rclone destination | `remote:path` | optional |
| qBittorrent Web UI URL | `http://qbittorrent-host:8080` | optional |

Secrets in this worksheet belong in your password manager or deployment vault,
not in git.

---

## 1. Install Docker

On a fresh Ubuntu server, install Docker Engine and the Compose plugin from
Docker's official packages or your distribution packages. Confirm both commands
work:

```bash
docker version
docker compose version
```

If your user cannot run Docker yet, either log out/in after joining the `docker`
group or run the commands below with `sudo docker ...`.

---

## 2. Create the Telegram API App

1. Open `https://my.telegram.org/apps` in a browser.
2. Sign in with the phone number that will own the app.
3. Create an app if you do not already have one.
4. Copy the values into your deployment notes:

| Telegram page value | CMLB value |
|---|---|
| `App api_id` | `TELEGRAM_API_ID` |
| `App api_hash` | `TELEGRAM_API_HASH` |

`api_hash` is secret. Do not paste it into chat, screenshots, logs, or commits.

---

## 3. Create the Bot With BotFather

Open Telegram and start a chat with `@BotFather`.

```text
/newbot
```

BotFather asks for:

| Prompt | What to enter |
|---|---|
| Bot name | Human display name, for example `CMLB Mirror Bot`. |
| Username | Must end in `bot`, for example `my_cmlb_bot`. |

BotFather replies with a token like:

```text
1234567890:AAExampleTokenValue
```

That token becomes `TELEGRAM_BOT_TOKEN`. Treat it like a password.

### Register BotFather Commands

Still in BotFather:

```text
/setcommands
```

Choose your bot, then paste this full command list:

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

Optional BotFather polish:

```text
/setdescription
/setabouttext
/setuserpic
```

Those only affect the Telegram profile; they are not required for CMLB to run.

---

## 4. Find Your Owner ID

Open Telegram and start a chat with `@userinfobot`. It replies with your numeric
Telegram user ID.

Copy that number into `OWNER_ID`. Do not use your username; CMLB needs the
numeric ID.

---

## 5. Clone CMLB and Create Local Files

```bash
git clone https://github.com/staneswilson/cpp-mirror-leech-bot.git cmlb
cd cmlb

cp config.example.json config.json
cp packaging/.env.example packaging/.env
```

Generate the aria2 RPC secret:

```bash
sed -i "s/^ARIA2_SECRET=.*/ARIA2_SECRET=$(openssl rand -hex 32)/" packaging/.env
```

Choose an image tag:

| Scenario | `CMLB_TAG` value |
|---|---|
| Deploy a published immutable image | `sha-<full commit sha from the Docker workflow>` |
| Deploy a release | `vX.Y.Z` |
| Build locally from this checkout | `local-$(git rev-parse --short=12 HEAD)` |

For a local build from the current checkout:

```bash
sed -i "s/^CMLB_TAG=.*/CMLB_TAG=local-$(git rev-parse --short=12 HEAD)/" packaging/.env
```

Production hosts should use a published release tag or `sha-<full commit>`.
Avoid floating tags such as `latest` and `main`.

---

## 6. Fill `packaging/.env`

Open the file:

```bash
nano packaging/.env
```

Set these lines:

```text
TELEGRAM_API_ID=123456
TELEGRAM_API_HASH=0123456789abcdef0123456789abcdef
TELEGRAM_BOT_TOKEN=1234567890:AAExampleTokenValue
OWNER_ID=987654321
ARIA2_SECRET=<already-generated-random-secret>
LOG_LEVEL=info
TZ=UTC
CMLB_IMAGE=ghcr.io/staneswilson/cpp-mirror-leech-bot
CMLB_TAG=sha-<full commit sha or release tag>
```

Rules for `.env`:

- Use one `NAME=value` per line.
- Do not put spaces around `=`.
- Do not wrap values in quotes.
- Never commit `packaging/.env`; it is gitignored.

Mapping:

| In `packaging/.env` | Injected into CMLB as | Config field |
|---|---|---|
| `TELEGRAM_API_ID` | `CMLB_TELEGRAM_API_ID` | `telegram.api_id` |
| `TELEGRAM_API_HASH` | `CMLB_TELEGRAM_API_HASH` | `telegram.api_hash` |
| `TELEGRAM_BOT_TOKEN` | `CMLB_TELEGRAM_BOT_TOKEN` | `telegram.bot_token` |
| `OWNER_ID` | `CMLB_TELEGRAM_OWNER_ID` | `telegram.owner_id` |
| `ARIA2_SECRET` | `CMLB_ARIA2_SECRET` | `aria2.secret` |
| `LOG_LEVEL` | `CMLB_LOGGING_LEVEL` | `logging.level` |

For a basic Telegram + aria2 deployment, you do not need to edit
`config.json` yet. Compose injects the required Telegram and aria2 values with
`CMLB_*` environment overrides.

---

## 7. Start CMLB

Validate the Compose file:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env config >/tmp/cmlb-compose.yml
```

Use one of the two startup paths below.

### Use a Published Image

Use this when `CMLB_TAG` is a release tag or `sha-<full commit>` that exists in
GitHub Container Registry:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb aria2
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build
```

### Build Locally

Use this when `CMLB_TAG` is a local tag or you intentionally want to compile the
image on this machine:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --build
```

The first local build compiles TDLib through vcpkg and can take 15-30 minutes.

Check the stack:

```bash
docker compose -f packaging/docker-compose.yml ps
docker compose -f packaging/docker-compose.yml logs -f cmlb
```

Healthy startup logs include:

```text
[info] CMLB 1.0.0 starting up.
[info] authentication_flow: bot authorised
[info] Bot ready. Awaiting Telegram updates.
```

Open Telegram and send these commands to your bot:

```text
/start
/ping
/version
/status
```

Expected verification:

| Command | Healthy result |
|---|---|
| `/start` | The bot replies, proving Telegram updates and replies work. |
| `/ping` | The bot returns latency. |
| `/version` | The bot returns the running build version. |
| `/status` | The bot returns a status summary without errors. |

---

## 8. Optional: Google Drive Setup

Do this only if you want Drive upload, clone, count, or delete.

1. Open Google Cloud Console.
2. Create a project, or choose an existing project.
3. Enable the Google Drive API for that project.
4. Create a service account.
5. Create a JSON key for the service account and download it.
6. Save it as `service_account.json` beside `config.json`.
7. Open the JSON file and copy `client_email`.
8. In Google Drive, create or choose the destination folder.
9. Share that folder with the service account `client_email` as Editor.
10. Copy the folder ID from the folder URL:

```text
https://drive.google.com/drive/folders/FOLDER_ID_IS_HERE
```

Set the Drive config in `config.json`:

```json
"google_drive": {
  "credentials_path": "/etc/cmlb/service_account.json",
  "parent_folder_id": "FOLDER_ID_IS_HERE",
  "use_service_accounts": true
}
```

For Docker, mount the key file by adding this under the `cmlb.volumes` list in
`packaging/docker-compose.yml`:

```yaml
      - ../service_account.json:/etc/cmlb/service_account.json:ro
```

Then restart:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d
```

Common Drive mistake: the service account is not your personal Drive account.
It cannot see a folder until that folder is explicitly shared with the
service-account email.

---

## 9. Optional: rclone Setup

Use rclone when your mirror target is not Google Drive or when you prefer
rclone's remote support.

On a trusted machine:

```bash
rclone config
rclone lsd remote:
```

Copy the generated config file to the server, for example:

```bash
mkdir -p ./rclone
cp ~/.config/rclone/rclone.conf ./rclone/rclone.conf
```

Set the config path in `config.json`:

```json
"rclone": {
  "executable": "rclone",
  "config_path": "/etc/cmlb/rclone.conf"
}
```

For Docker, mount it:

```yaml
      - ../rclone/rclone.conf:/etc/cmlb/rclone.conf:ro
```

CMLB uploads to the saved per-user rclone destination, formatted like
`remote:path`. The current `/settings` panel cycles the mirror destination;
text entry for the rclone path is operator-managed in v1.

---

## 10. Optional: qBittorrent Setup

Use qBittorrent for `/qbmirror` and `/qbleech`. The default Compose file starts
aria2 only; qBittorrent can run on the host or on another container.

Minimum CMLB config:

```json
"qbittorrent": {
  "url": "http://qbittorrent-host:8080",
  "username": "admin",
  "password": "REPLACE_WITH_WEB_UI_PASSWORD"
}
```

If qBittorrent runs on the Docker host, use a URL reachable from the CMLB
container. On Linux Docker, `http://host.docker.internal:8080` works only when
that host alias is configured; otherwise use the host bridge IP or put
qBittorrent on the same Compose network.

---

## Stop, Restart, Upgrade

Stop:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env down
```

Restart after editing config:

```bash
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d
```

Upgrade to a newer published image:

```bash
git pull --ff-only
nano packaging/.env    # set CMLB_TAG to the new release or sha-<full commit>
docker compose -f packaging/docker-compose.yml --env-file packaging/.env pull cmlb
docker compose -f packaging/docker-compose.yml --env-file packaging/.env up -d --no-build
```

Data, logs, TDLib state, and downloads live in Docker volumes, so recreating
containers does not wipe the bot.

---

## Troubleshooting

| Symptom | First check |
|---|---|
| `CMLB_TAG must be pinned` | Set `CMLB_TAG` in `packaging/.env`. |
| Bot does not reply | `docker compose -f packaging/docker-compose.yml logs -f cmlb` |
| Config validation fails | Recheck `TELEGRAM_*`, `OWNER_ID`, and `ARIA2_SECRET`. |
| `api_id` or `owner_id` rejected | They must be real non-zero numbers, not placeholders. |
| Downloads fail immediately | Confirm the `aria2` service is healthy in `docker compose ... ps`. |
| Drive upload returns 404/403 | Share the target Drive folder with the service-account email. |
| qBittorrent commands fail | Confirm the Web UI URL is reachable from inside the CMLB container. |

Full operational procedures are in [`runbook.md`](runbook.md). Every config
field and environment override is documented in
[`configuration_reference.md`](configuration_reference.md).
