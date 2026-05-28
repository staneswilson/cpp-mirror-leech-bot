#!/usr/bin/env bash
# Bootstraps a Linux development environment for CMLB.
#
# Installs system packages, clones vcpkg if missing, and prints the next steps.
# Idempotent — safe to re-run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-${HOME}/vcpkg}"

log() { printf '\033[1;34m==> \033[0m%s\n' "$*"; }
err() { printf '\033[1;31merr:\033[0m %s\n' "$*" >&2; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        err "missing required command: $1"
        return 1
    fi
}

install_apt_packages() {
    log "Installing system packages via apt"
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential \
        gcc-13 g++-13 \
        clang-17 clang-tidy-17 clang-format-17 \
        cmake ninja-build \
        git curl ca-certificates pkg-config \
        autoconf automake libtool \
        zip unzip tar \
        python3 python3-pip \
        aria2 ffmpeg p7zip-full
}

clone_vcpkg() {
    if [[ -d "${VCPKG_ROOT}/.git" ]]; then
        log "vcpkg already present at ${VCPKG_ROOT}"
        return
    fi
    log "Cloning vcpkg into ${VCPKG_ROOT}"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
}

setup_pre_commit() {
    if ! command -v pre-commit >/dev/null 2>&1; then
        log "Installing pre-commit"
        pip3 install --user pre-commit
    fi
    if [[ -f "${REPO_ROOT}/.pre-commit-config.yaml" ]]; then
        log "Installing pre-commit hooks"
        ( cd "${REPO_ROOT}" && pre-commit install )
    fi
}

print_next_steps() {
    cat <<EOF

Bootstrap complete.

Next steps:
  export VCPKG_ROOT=${VCPKG_ROOT}
  export CC=gcc-13 CXX=g++-13

  cmake --preset debug
  cmake --build --preset debug
  ctest --preset debug --output-on-failure

To switch to clang:
  export CC=clang-17 CXX=clang++-17

EOF
}

main() {
    require_cmd sudo
    require_cmd apt-get
    install_apt_packages
    clone_vcpkg
    setup_pre_commit
    print_next_steps
}

main "$@"
