#!/usr/bin/env bash
# Formats every source file in the repo via clang-format + cmake-format.
# Run from the repository root.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
CMAKE_FORMAT="${CMAKE_FORMAT:-cmake-format}"

if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
    printf 'err: clang-format not found (set CLANG_FORMAT env var to override)\n' >&2
    exit 1
fi
if ! command -v "${CMAKE_FORMAT}" >/dev/null 2>&1; then
    printf 'err: cmake-format not found (set CMAKE_FORMAT env var to override)\n' >&2
    exit 1
fi

printf '==> Running clang-format on C++ sources\n'
mapfile -t CPP_FILES < <(git ls-files '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' \
    -- ':!:*.legacy.*' \
       ':!:legacy/' \
       ':!:third_party/')
if (( ${#CPP_FILES[@]} > 0 )); then
    "${CLANG_FORMAT}" -i --style=file "${CPP_FILES[@]}"
fi

printf '==> Running cmake-format on CMake files\n'
mapfile -t CMAKE_FILES < <(git ls-files '*.cmake' 'CMakeLists.txt' '**/CMakeLists.txt' \
    -- ':!:*.legacy.*')
if (( ${#CMAKE_FILES[@]} > 0 )); then
    "${CMAKE_FORMAT}" -i "${CMAKE_FILES[@]}"
fi

printf '==> Formatting complete\n'
