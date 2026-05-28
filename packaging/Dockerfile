# syntax=docker/dockerfile:1.9
#
# Multi-stage, multi-arch Docker build for CMLB.
#
#   * Builder       — Ubuntu 24.04, GCC 14, vcpkg-managed deps, ccache.
#   * Test (gated)  — runs ctest in the builder before promoting to runtime.
#   * Runtime       — Debian 13 slim, runs as non-root, ships with aria2c +
#                     ffmpeg + p7zip-full + rclone so /mirror, /leech and
#                     auto-extract work out of the box.
#
# Build (single-arch):
#     docker build -t ghcr.io/staneswilson/cmlb:latest -f packaging/Dockerfile .
#
# Build (multi-arch, requires buildx):
#     docker buildx build --platform linux/amd64,linux/arm64 \
#         -t ghcr.io/staneswilson/cmlb:latest \
#         --push \
#         -f packaging/Dockerfile .
#
# Run:
#     docker run --rm \
#         -v $(pwd)/config.json:/etc/cmlb/config.json:ro \
#         -v cmlb-data:/var/lib/cmlb \
#         -v cmlb-logs:/var/log/cmlb \
#         ghcr.io/staneswilson/cmlb:latest

ARG UBUNTU_VERSION=24.04
ARG DEBIAN_VERSION=13-slim
ARG VCPKG_BASELINE=2b65c20fc66eda893aa15a15a453c3cf09500b19
ARG CMLB_BUILD_TYPE=Release
ARG CMLB_RUN_TESTS=1

# =============================================================================
# Stage 1: builder — full toolchain + vcpkg
# =============================================================================
FROM ubuntu:${UBUNTU_VERSION} AS builder

ARG VCPKG_BASELINE
ARG CMLB_BUILD_TYPE
ARG CMLB_RUN_TESTS

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    CMAKE_BUILD_PARALLEL_LEVEL=4 \
    CC=gcc-14 \
    CXX=g++-14

# Pinned ubuntu mirror for reproducibility, plus build essentials and the
# libraries that vcpkg's port build scripts shell out to (perl, autotools,
# pkg-config, zip/unzip/tar, python3 for ports that use it).
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 \
        cmake ninja-build ccache \
        git curl wget ca-certificates pkg-config gnupg lsb-release \
        autoconf autoconf-archive automake libtool \
        zip unzip tar xz-utils bzip2 \
        python3 python3-jinja2 \
        gperf bison flex \
        linux-libc-dev libstdc++-14-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# Clone vcpkg at the exact baseline used by `vcpkg.json`. Doing this in its
# own layer means manifest-only changes don't reclone the registry.
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
    && git -C "${VCPKG_ROOT}" checkout "${VCPKG_BASELINE}" \
    && "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src

# Manifest-first layer: copy only the vcpkg manifest so port installs are
# cached when source-only changes happen. The bind-mount cache stores the
# resolved port archives between builds.
COPY vcpkg.json vcpkg-configuration.json ./
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    "${VCPKG_ROOT}/vcpkg" install --triplet x64-linux --x-manifest-root=/src \
        --x-feature=telegram

# Now copy the rest of the source tree. Anything in .dockerignore stays out.
COPY . .

# ccache cuts incremental rebuilds to seconds.
ENV PATH="/usr/lib/ccache:${PATH}" \
    CCACHE_DIR=/root/.cache/ccache \
    CCACHE_MAXSIZE=2G

# Configure + build + (optionally) run tests as a gate. A failed test halts
# the image from being promoted to the runtime stage.
RUN --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    cmake -S . -B build/release -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMLB_BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DCMLB_BUILD_TESTS="$([ "${CMLB_RUN_TESTS}" = "1" ] && echo ON || echo OFF)" \
        -DCMLB_WITH_TELEGRAM=ON \
    && cmake --build build/release --parallel \
    && if [ "${CMLB_RUN_TESTS}" = "1" ]; then \
            ctest --test-dir build/release --output-on-failure --no-tests=error; \
        fi \
    && cmake --install build/release --prefix /opt/cmlb --strip

# =============================================================================
# Stage 2: runtime — Debian slim with the external tools the bot shells out to
# =============================================================================
FROM debian:${DEBIAN_VERSION} AS runtime

ARG DEBIAN_VERSION
ARG VCPKG_BASELINE
ARG CMLB_BUILD_TYPE
ARG GIT_SHA=unknown
ARG BUILD_DATE=unknown

LABEL org.opencontainers.image.title="cmlb" \
      org.opencontainers.image.description="Production-grade C++23 Telegram mirror/leech bot" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.source="https://github.com/staneswilson/cpp-mirror-leech-bot" \
      org.opencontainers.image.url="https://github.com/staneswilson/cpp-mirror-leech-bot" \
      org.opencontainers.image.documentation="https://github.com/staneswilson/cpp-mirror-leech-bot/blob/main/docs/runbook.md" \
      org.opencontainers.image.vendor="CMLB" \
      org.opencontainers.image.authors="cmlb maintainers" \
      org.opencontainers.image.revision="${GIT_SHA}" \
      org.opencontainers.image.created="${BUILD_DATE}" \
      org.opencontainers.image.base.name="docker.io/library/debian:${DEBIAN_VERSION}"

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    CMLB_LOGGING_LOGS_DIR=/var/log/cmlb \
    CMLB_PATHS_DATA_DIR=/var/lib/cmlb \
    CMLB_PATHS_DOWNLOAD_DIR=/var/lib/cmlb/downloads

# Runtime libraries that CMLB links against + the external tools the bot
# shells out to. tini gives us PID-1 signal handling + zombie reaping.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tini tzdata \
        libstdc++6 libgcc-s1 \
        libssl3 libsqlite3-0 \
        aria2 ffmpeg p7zip-full rclone \
    && rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*

# Create a dedicated `cmlb` system user. UID/GID 10001 keeps it well clear of
# host UIDs to avoid bind-mount permission surprises.
RUN groupadd --system --gid 10001 cmlb \
    && useradd  --system --uid 10001 --gid cmlb \
                --home-dir /var/lib/cmlb --shell /usr/sbin/nologin \
                --comment "cmlb mirror/leech bot" cmlb \
    && mkdir -p /etc/cmlb /var/lib/cmlb/downloads /var/log/cmlb \
    && chown -R cmlb:cmlb /etc/cmlb /var/lib/cmlb /var/log/cmlb

COPY --from=builder /opt/cmlb/bin/cmlb /usr/local/bin/cmlb
COPY --from=builder /src/config.example.json /etc/cmlb/config.example.json
COPY --from=builder /src/LICENSE /usr/share/doc/cmlb/LICENSE

# Verify the binary is dynamically linked correctly before shipping. `ldd`
# returns 0 only if every NEEDED library resolves.
RUN /usr/local/bin/cmlb --version && ldd /usr/local/bin/cmlb >/dev/null

USER cmlb:cmlb
WORKDIR /var/lib/cmlb

# Liveness probe: parse the config — if the bot can't validate its config, it
# shouldn't be marked healthy. Compose/k8s map this onto restart logic.
HEALTHCHECK --interval=60s --timeout=10s --start-period=15s --retries=3 \
    CMD /usr/local/bin/cmlb --validate-config /etc/cmlb/config.json || exit 1

VOLUME ["/etc/cmlb", "/var/lib/cmlb", "/var/log/cmlb"]

# tini reaps zombies left behind by `aria2c` / `ffmpeg` / `7z` subprocesses
# and forwards SIGTERM cleanly so graceful shutdown works in compose/k8s.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/cmlb"]
CMD ["/etc/cmlb/config.json"]
