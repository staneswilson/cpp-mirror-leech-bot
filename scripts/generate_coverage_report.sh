#!/usr/bin/env bash
# Builds with the coverage preset, runs the tests, and emits an HTML report.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

PRESET="${PRESET:-coverage}"
BUILD_DIR="build/${PRESET}"
OUTPUT_DIR="${BUILD_DIR}/coverage_html"

log() { printf '\033[1;34m==> \033[0m%s\n' "$*"; }

log "Configuring with preset ${PRESET}"
cmake --preset "${PRESET}"

log "Building"
cmake --build --preset "${PRESET}" --parallel

log "Running tests under coverage instrumentation"
ctest --preset "${PRESET}" --output-on-failure

log "Generating coverage report"
mkdir -p "${OUTPUT_DIR}"

if command -v llvm-cov >/dev/null 2>&1 && command -v llvm-profdata >/dev/null 2>&1; then
    # Clang flow
    llvm-profdata merge -sparse "${BUILD_DIR}"/*.profraw -o "${BUILD_DIR}/coverage.profdata"
    llvm-cov show "${BUILD_DIR}/bin/"* \
        -instr-profile="${BUILD_DIR}/coverage.profdata" \
        -format=html -output-dir="${OUTPUT_DIR}" \
        -ignore-filename-regex='(tests/|third_party/|build/)'
elif command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
    # GCC flow
    lcov --directory "${BUILD_DIR}" --capture --output-file "${BUILD_DIR}/coverage.info"
    lcov --remove "${BUILD_DIR}/coverage.info" \
        '*/tests/*' '*/third_party/*' '*/build/*' '/usr/*' \
        --output-file "${BUILD_DIR}/coverage.info.filtered"
    genhtml "${BUILD_DIR}/coverage.info.filtered" --output-directory "${OUTPUT_DIR}"
else
    printf 'err: neither llvm-cov+llvm-profdata nor lcov+genhtml is available\n' >&2
    exit 1
fi

log "Coverage report: ${OUTPUT_DIR}/index.html"
