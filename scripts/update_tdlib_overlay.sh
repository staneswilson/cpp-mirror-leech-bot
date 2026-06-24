#!/usr/bin/env bash
# Updates the repo-owned TDLib vcpkg overlay to an exact upstream commit.
#
# Usage:
#   scripts/update_tdlib_overlay.sh
#   TDLIB_REF=<commit-sha> scripts/update_tdlib_overlay.sh
#
# The default tracks tdlib/td master at the time the script is run, computes
# the archive SHA512, reads TDLib's CMake project version, and updates:
#   - vcpkg-overlays/tdlib/portfile.cmake
#   - vcpkg-overlays/tdlib/vcpkg.json
#   - vcpkg.json override
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TDLIB_REPO_URL="https://github.com/tdlib/td.git"
TDLIB_RAW_URL="https://raw.githubusercontent.com/tdlib/td"
TDLIB_ARCHIVE_URL="https://github.com/tdlib/td/archive"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$1" >&2
        exit 1
    fi
}

sha512_file() {
    if command -v sha512sum >/dev/null 2>&1; then
        sha512sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 512 "$1" | awk '{print $1}'
    else
        printf 'missing required command: sha512sum or shasum\n' >&2
        exit 1
    fi
}

require_cmd awk
require_cmd curl
require_cmd git
require_cmd python3

tdlib_ref="${TDLIB_REF:-}"
if [[ -z "${tdlib_ref}" ]]; then
    tdlib_ref="$(git ls-remote "${TDLIB_REPO_URL}" refs/heads/master | awk '{print $1}')"
fi

if [[ ! "${tdlib_ref}" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'TDLIB_REF must resolve to a full 40-character commit SHA, got: %s\n' "${tdlib_ref}" >&2
    exit 1
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

cmake_file="${tmp_dir}/CMakeLists.txt"
archive_file="${tmp_dir}/tdlib-${tdlib_ref}.tar.gz"

curl --fail --location --silent --show-error \
    "${TDLIB_RAW_URL}/${tdlib_ref}/CMakeLists.txt" \
    --output "${cmake_file}"

tdlib_version="$(
    python3 - "${cmake_file}" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r"project\(TDLib\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s+", text)
if not match:
    raise SystemExit("failed to read TDLib project version")
print(match.group(1))
PY
)"

curl --fail --location --silent --show-error \
    "${TDLIB_ARCHIVE_URL}/${tdlib_ref}.tar.gz" \
    --output "${archive_file}"
tdlib_sha512="$(sha512_file "${archive_file}")"

python3 - "${REPO_ROOT}" "${tdlib_ref}" "${tdlib_sha512}" "${tdlib_version}" <<'PY'
import pathlib
import re
import sys

repo = pathlib.Path(sys.argv[1])
tdlib_ref = sys.argv[2]
tdlib_sha512 = sys.argv[3]
tdlib_version = sys.argv[4]

portfile = repo / "vcpkg-overlays" / "tdlib" / "portfile.cmake"
text = portfile.read_text(encoding="utf-8")
text = re.sub(r"REF [0-9a-f]{40}", f"REF {tdlib_ref}", text)
text = re.sub(r"SHA512 [0-9a-f]{128}", f"SHA512 {tdlib_sha512}", text)
portfile.write_text(text, encoding="utf-8")

overlay_manifest = repo / "vcpkg-overlays" / "tdlib" / "vcpkg.json"
text = overlay_manifest.read_text(encoding="utf-8")
text = re.sub(r'("version"\s*:\s*")[^"]+(")', rf"\g<1>{tdlib_version}\2", text, count=1)
overlay_manifest.write_text(text, encoding="utf-8")

manifest = repo / "vcpkg.json"
text = manifest.read_text(encoding="utf-8")
updated, count = re.subn(
    r'("name"\s*:\s*"tdlib"\s*,\s*"version"\s*:\s*")[^"]+(")',
    rf"\g<1>{tdlib_version}\2",
    text,
    count=1,
    flags=re.DOTALL,
)
if count != 1:
    raise SystemExit("failed to update tdlib override in vcpkg.json")
manifest.write_text(updated, encoding="utf-8")
PY

printf 'TDLib overlay updated to %s %s\n' "${tdlib_version}" "${tdlib_ref}"
printf 'Archive SHA512: %s\n' "${tdlib_sha512}"
