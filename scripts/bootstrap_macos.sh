#!/usr/bin/env bash
# Bootstraps a macOS development environment for CMLB.
# Uses Homebrew for system packages.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-${HOME}/vcpkg}"

log() { printf '\033[1;34m==> \033[0m%s\n' "$*"; }
err() { printf '\033[1;31merr:\033[0m %s\n' "$*" >&2; }

ensure_homebrew() {
    if ! command -v brew >/dev/null 2>&1; then
        err "Homebrew not found. Install from https://brew.sh first."
        exit 1
    fi
}

install_brew_packages() {
    log "Installing system packages via Homebrew"
    brew update
    brew install \
        llvm@17 \
        cmake ninja \
        git pkg-config \
        autoconf automake libtool \
        python@3.12 \
        aria2 ffmpeg p7zip
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
        pip3 install --user pre-commit || brew install pre-commit
    fi
    if [[ -f "${REPO_ROOT}/.pre-commit-config.yaml" ]]; then
        log "Installing pre-commit hooks"
        ( cd "${REPO_ROOT}" && pre-commit install )
    fi
}

print_next_steps() {
    local llvm_prefix
    llvm_prefix="$(brew --prefix llvm@17)"
    cat <<EOF

Bootstrap complete.

Next steps:
  export VCPKG_ROOT=${VCPKG_ROOT}
  export PATH="${llvm_prefix}/bin:\${PATH}"
  export CC="${llvm_prefix}/bin/clang"
  export CXX="${llvm_prefix}/bin/clang++"

  cmake --preset debug
  cmake --build --preset debug
  ctest --preset debug --output-on-failure

EOF
}

main() {
    ensure_homebrew
    install_brew_packages
    clone_vcpkg
    setup_pre_commit
    print_next_steps
}

main "$@"
