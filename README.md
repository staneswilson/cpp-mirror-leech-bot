# CMLB - C++ Mirror/Leech Bot

A high-performance Telegram Mirror/Leech bot written in modern C++23.

## Features

- **Mirror**: Download files from URLs using Aria2c and upload to cloud storage
- **Leech**: Download files and upload directly to Telegram
- **QBittorrent**: Support for torrent downloads
- **RSS Feeds**: Automatic monitoring and downloading from RSS feeds
- **Archive Support**: Extract and compress archives (7z, zip, tar, rar)
- **Media Processing**: Thumbnail extraction, sample video generation
- **Permission System**: 4-tier access control (Anyone, User, Admin, Owner)

## Requirements

### Dependencies

- **C++23 Compiler**: GCC 13+, Clang 17+, or MSVC 2022
- **CMake**: 3.28+
- **Boost**: Beast, Asio, System
- **nlohmann_json**: 3.11.2+
- **TDLib**: Telegram Database Library
- **OpenSSL**: For HTTPS RSS feeds

### External Programs

- **Aria2c**: Download daemon (must be running)
- **FFmpeg**: Media processing (optional)
- **7-Zip**: Archive extraction (optional)
- **rclone**: Cloud upload (optional)

## Build

```bash
# Clone repository
git clone https://github.com/yourusername/cmlb.git
cd cmlb

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Run
./build/bin/cmlb_bot
```

## Configuration

1. Copy `config.example.json` to `config.json`
2. Fill in your Telegram API credentials
3. Set your owner user ID

Environment variables override config file:

| Variable | Description |
|----------|-------------|
| `TELEGRAM_API_ID` | Telegram API ID from my.telegram.org |
| `TELEGRAM_API_HASH` | Telegram API hash |
| `OWNER_ID` | Your Telegram user ID |
| `ARIA2_RPC_URL` | Aria2 WebSocket URL |
| `ARIA2_SECRET` | Aria2 RPC secret |

## Commands

### Mirror/Leech
- `/mirror <url>` - Mirror to cloud storage
- `/leech <url>` - Leech to Telegram
- `/qbmirror <url>` - Mirror using QBittorrent
- `/qbleech <url>` - Leech using QBittorrent
- `/clone <gdrive_link>` - Clone Google Drive file

### Status/Control
- `/status` - Show active downloads
- `/cancel <gid>` - Cancel a download
- `/cancelall` - Cancel all downloads (Admin)
- `/pause <gid>` - Pause a download
- `/resume <gid>` - Resume a download

### Settings
- `/settings` - User settings
- `/botsettings` - Bot settings (Admin)

### Utility
- `/help` - Show help
- `/ping` - Check latency
- `/stats` - Bot statistics
- `/log` - Get log file (Owner)

## Architecture

```
cmlb/
├── include/
│   ├── core/           # Core engine and configuration
│   ├── commands/       # Command routing
│   ├── downloaders/    # Download clients (Aria2, QBit)
│   ├── uploaders/      # Upload clients (Telegram, Rclone)
│   ├── db/             # Database layer
│   └── utils/          # Utilities (RSS, Archive, Media)
├── src/
│   └── ...             # Implementation files
├── CMakeLists.txt
└── config.example.json
```

## License

MIT License - see [LICENSE](LICENSE)
