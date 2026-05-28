#!/usr/bin/env bash
# Runs clang-tidy across the project, using the compile_commands.json from the
# debug preset. Filters to changed files when run with --diff against a base.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${PRESET:-debug}"
BUILD_DIR="build/${PRESET}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
DIFF_BASE=""
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

usage() {
    cat <<EOF
Usage: $0 [--diff <base-ref>] [--preset <name>]

Options:
  --diff <ref>     Analyze only files changed since <ref> (e.g. origin/main).
  --preset <name>  CMake preset to read compile_commands.json from (default: debug).
  -h, --help       Show this help.
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --diff)   DIFF_BASE="$2"; shift 2;;
        --preset) PRESET="$2"; BUILD_DIR="build/${PRESET}"; shift 2;;
        -h|--help) usage; exit 0;;
        *) printf 'err: unknown argument: %s\n' "$1" >&2; usage; exit 2;;
    esac
done

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
    printf 'err: %s/compile_commands.json missing. Run: cmake --preset %s\n' \
        "${BUILD_DIR}" "${PRESET}" >&2
    exit 1
fi

if [[ -n "${DIFF_BASE}" ]]; then
    printf '==> Analyzing files changed since %s\n' "${DIFF_BASE}"
    mapfile -t FILES < <(git diff --name-only "${DIFF_BASE}" HEAD -- 'src/*.cpp' 'tests/*.cpp')
else
    printf '==> Analyzing all production sources\n'
    mapfile -t FILES < <(git ls-files 'src/*.cpp' 'tests/*.cpp' \
        -- ':!:*.legacy.*' ':!:src/commands' ':!:src/db' ':!:src/downloaders' \
           ':!:src/uploaders' ':!:src/utils' ':!:src/core/bot_engine*' \
           ':!:src/core/message_sender*')
fi

if (( ${#FILES[@]} == 0 )); then
    printf '==> No files to analyze\n'
    exit 0
fi

printf '==> Running clang-tidy on %d file(s) with %s parallel jobs\n' "${#FILES[@]}" "${JOBS}"
printf '%s\n' "${FILES[@]}" | xargs -n1 -P"${JOBS}" \
    "${CLANG_TIDY}" -p "${BUILD_DIR}" --warnings-as-errors='*' \
                    --header-filter='^.*include/cmlb/.*\.hpp$'
