# syntax=docker/dockerfile:1.9
#
# Multi-stage, multi-arch Docker build for CMLB.
#
#   * Builder       — Ubuntu 24.04, GCC 14, vcpkg-managed deps, ccache.
#   * Test (gated)  — runs ctest in the builder before promoting to runtime.
#   * Runtime       — Debian 13 slim, runs as non-root, ships with runtime
#                     libraries and pinned external tools (aria2c, ffmpeg,
#                     upstream 7-Zip, upstream rclone). /mirror and /leech
#                     still require reachable aria2/qBittorrent RPC;
#                     archive/media commands are wired only when the
#                     presentation layer exposes them.
#
# Build (single-arch):
#     docker build -t ghcr.io/staneswilson/cpp-mirror-leech-bot:local -f packaging/Dockerfile .
#
# Build (multi-arch, requires buildx):
#     docker buildx build --platform linux/amd64,linux/arm64 \
#         -t ghcr.io/staneswilson/cpp-mirror-leech-bot:sha-<full-commit> \
#         --push \
#         -f packaging/Dockerfile .
#
# Run:
#     docker run --rm \
#         -v $(pwd)/config.json:/etc/cmlb/config.json:ro \
#         -v cmlb-data:/var/lib/cmlb \
#         -v cmlb-logs:/var/log/cmlb \
#         ghcr.io/staneswilson/cpp-mirror-leech-bot:sha-<full-commit>

ARG UBUNTU_VERSION=24.04
ARG DEBIAN_VERSION=13-slim
ARG VCPKG_BASELINE=a7eda31dc16994fcaa8587982eb833a8695f1b6f
ARG CMLB_BUILD_TYPE=Release
ARG CMLB_RUN_TESTS=1
ARG RCLONE_VERSION=1.74.3
ARG SEVEN_ZIP_VERSION=26.01

# =============================================================================
# Stage 1: builder — full toolchain + vcpkg
# =============================================================================
FROM ubuntu:${UBUNTU_VERSION} AS builder

# Buildkit injects TARGETARCH on every multi-arch build (linux/amd64 → "amd64",
# linux/arm64 → "arm64"). We translate that to the matching vcpkg triplet so a
# cross-arch buildx job doesn't try to compile x64 binaries on an aarch64 host.
ARG TARGETARCH
ARG VCPKG_BASELINE
ARG CMLB_BUILD_TYPE
ARG CMLB_RUN_TESTS
ARG RCLONE_VERSION
ARG SEVEN_ZIP_VERSION

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
#
# TARGETARCH is injected by buildx (amd64 / arm64). We translate to the
# matching vcpkg triplet so a multi-arch build doesn't try to compile x64
# binaries on an aarch64 host. The triplet is recorded to /opt/cmlb/triplet
# so the later configure step can read it back without recomputing.
COPY vcpkg.json vcpkg-configuration.json ./
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    mkdir -p /opt/cmlb \
 && case "${TARGETARCH:-amd64}" in \
        amd64) triplet=x64-linux ;; \
        arm64) triplet=arm64-linux ;; \
        *) echo "Unsupported TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac \
 && echo "${triplet}" > /opt/cmlb/triplet \
 && "${VCPKG_ROOT}/vcpkg" install --triplet "${triplet}" --x-manifest-root=/src \
        --x-feature=telegram

# Now copy the rest of the source tree. Anything in .dockerignore stays out.
COPY . .

# ccache cuts incremental rebuilds to seconds.
ENV PATH="/usr/lib/ccache:${PATH}" \
    CCACHE_DIR=/root/.cache/ccache \
    CCACHE_MAXSIZE=2G

# Configure + build + (optionally) run tests as a gate. A failed test halts
# the image from being promoted to the runtime stage.
#
# Two things must line up with the earlier `vcpkg install --x-feature=telegram`
# step or the toolchain re-runs install at configure time WITHOUT the telegram
# feature and overwrites our tdlib-bearing manifest dir:
#   * VCPKG_TARGET_TRIPLET — read from the file we wrote above so it matches
#     the host arch (x64-linux on amd64, arm64-linux on arm64).
#   * VCPKG_MANIFEST_FEATURES — must include `telegram` so the toolchain
#     keeps tdlib in the install set.
RUN --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    triplet="$(cat /opt/cmlb/triplet)" \
 && cmake -S . -B build/release -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMLB_BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="${triplet}" \
        -DVCPKG_MANIFEST_FEATURES="telegram" \
        -DCMLB_BUILD_TESTS="$([ "${CMLB_RUN_TESTS}" = "1" ] && echo ON || echo OFF)" \
        -DCMLB_WITH_TELEGRAM=ON \
    && cmake --build build/release --parallel \
    && if [ "${CMLB_RUN_TESTS}" = "1" ]; then \
            ctest --test-dir build/release --output-on-failure --no-tests=error; \
        fi \
    && cmake --install build/release --prefix /opt/cmlb --strip

# Pinned upstream runtime tools. Debian's rclone/p7zip packages lag upstream
# significantly; installing the official static binaries avoids shipping old
# packages with known CVEs while keeping the runtime image reproducible.
RUN case "${TARGETARCH:-amd64}" in \
        amd64) \
            rclone_arch=amd64; \
            rclone_sha=dbee7ccd7a5d617e4ed4cd4555c16669b511abfe8d31164f61be35ac9e999bd2; \
            seven_zip_asset=7z2601-linux-x64.tar.xz; \
            seven_zip_sha=8ea0fc8a135e7b848e80a4116fe22dff56c8c4518dde1f43cce67f4e340b437a ;; \
        arm64) \
            rclone_arch=arm64; \
            rclone_sha=8f8d47446e061f80c3256659fe8e21f56d72d96aaefe1275d088ea5eb6b42aa7; \
            seven_zip_asset=7z2601-linux-arm64.tar.xz; \
            seven_zip_sha=39f8c9070c300a63c7484d9a983119ef3edf841e1ddf69f1affae29fdec5f612 ;; \
        *) echo "Unsupported TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac \
 && mkdir -p /opt/cmlb/tools /tmp/cmlb-tools \
 && curl -fsSLo /tmp/cmlb-tools/rclone.zip \
        "https://github.com/rclone/rclone/releases/download/v${RCLONE_VERSION}/rclone-v${RCLONE_VERSION}-linux-${rclone_arch}.zip" \
 && echo "${rclone_sha}  /tmp/cmlb-tools/rclone.zip" | sha256sum -c - \
 && unzip -q /tmp/cmlb-tools/rclone.zip -d /tmp/cmlb-tools \
 && install -m 0755 \
        "/tmp/cmlb-tools/rclone-v${RCLONE_VERSION}-linux-${rclone_arch}/rclone" \
        /opt/cmlb/tools/rclone \
 && curl -fsSLo /tmp/cmlb-tools/7zip.tar.xz \
        "https://github.com/ip7z/7zip/releases/download/${SEVEN_ZIP_VERSION}/${seven_zip_asset}" \
 && echo "${seven_zip_sha}  /tmp/cmlb-tools/7zip.tar.xz" | sha256sum -c - \
 && mkdir -p /tmp/cmlb-tools/7zip \
 && tar -xJf /tmp/cmlb-tools/7zip.tar.xz -C /tmp/cmlb-tools/7zip \
 && install -m 0755 /tmp/cmlb-tools/7zip/7zz /opt/cmlb/tools/7zz \
 && ln -s /opt/cmlb/tools/7zz /opt/cmlb/tools/7z \
 && rm -rf /tmp/cmlb-tools

# =============================================================================
# Stage 2: runtime — Debian slim with the external tools the bot shells out to
# =============================================================================
FROM debian:${DEBIAN_VERSION} AS runtime

ARG DEBIAN_VERSION
ARG VCPKG_BASELINE
ARG CMLB_BUILD_TYPE
ARG RCLONE_VERSION
ARG SEVEN_ZIP_VERSION
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
      org.opencontainers.image.base.name="docker.io/library/debian:${DEBIAN_VERSION}" \
      org.opencontainers.image.version.rclone="${RCLONE_VERSION}" \
      org.opencontainers.image.version.7zip="${SEVEN_ZIP_VERSION}"

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
        aria2 ffmpeg \
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
COPY --from=builder /opt/cmlb/tools/7zz /usr/local/bin/7zz
COPY --from=builder /opt/cmlb/tools/rclone /usr/local/bin/rclone
COPY --from=builder /src/config.example.json /etc/cmlb/config.example.json
COPY --from=builder /src/LICENSE /usr/share/doc/cmlb/LICENSE

# Verify the binary is dynamically linked correctly before shipping. `ldd`
# returns 0 only if every NEEDED library resolves.
RUN ln -s /usr/local/bin/7zz /usr/local/bin/7z \
    && /usr/local/bin/cmlb --version \
    && ldd /usr/local/bin/cmlb >/dev/null \
    && /usr/local/bin/rclone version \
    && /usr/local/bin/7z | head -3

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
