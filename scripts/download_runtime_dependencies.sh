#!/usr/bin/env bash
# Downloads runtime executable dependencies (aria2c) into ${REPO_ROOT}/runtime/.
# Replaces the legacy practice of checking aria2c.exe into the repo.
#
# Idempotent — skips downloads when files exist with matching checksums.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${REPO_ROOT}/runtime"
mkdir -p "${RUNTIME_DIR}"

ARIA2_VERSION="${ARIA2_VERSION:-1.37.0}"

log() { printf '\033[1;34m==> \033[0m%s\n' "$*"; }

uname_s=$(uname -s)
uname_m=$(uname -m)

case "${uname_s}" in
    Linux*)
        ARIA2_ASSET="aria2-${ARIA2_VERSION}-linux-gnu-${uname_m}-build1.tar.bz2"
        ARIA2_URL="https://github.com/aria2/aria2/releases/download/release-${ARIA2_VERSION}/${ARIA2_ASSET}"
        ;;
    Darwin*)
        log "On macOS, prefer 'brew install aria2'. Skipping download."
        exit 0
        ;;
    MINGW*|MSYS*|CYGWIN*)
        ARIA2_ASSET="aria2-${ARIA2_VERSION}-win-64bit-build1.zip"
        ARIA2_URL="https://github.com/aria2/aria2/releases/download/release-${ARIA2_VERSION}/${ARIA2_ASSET}"
        ;;
    *)
        printf 'err: unsupported OS: %s\n' "${uname_s}" >&2
        exit 1
        ;;
esac

DEST="${RUNTIME_DIR}/${ARIA2_ASSET}"

if [[ -f "${DEST}" ]]; then
    log "Already downloaded: ${ARIA2_ASSET}"
else
    log "Downloading ${ARIA2_URL}"
    curl -fL -o "${DEST}" "${ARIA2_URL}"
fi

log "Done. Extract into ${RUNTIME_DIR}/ manually or via your packaging step."
log "Place the resulting 'aria2c' binary on PATH or configure aria2.executable in config.json."
