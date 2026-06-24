# ADR-0013: Pin TDLib Through a Repository Overlay Port

- **Status:** Accepted
- **Date:** 2026-06-24
- **Deciders:** Engineering team

## Context

CMLB's Telegram boundary is compiled against TDLib generated headers. New
Telegram features can appear in upstream TDLib before the public vcpkg registry
updates its `tdlib` port. That lag matters for this project because CI must
compile the real Telegram gateway and catch API drift before deployment.

The registry baseline still pins the rest of the dependency graph through
`vcpkg-configuration.json`, but TDLib needs a tighter pin: exact upstream
commit, exact archive hash, and a local port definition that CI, Docker, and
developer machines all consume.

## Decision

CMLB uses a repository-owned vcpkg overlay for TDLib under
`vcpkg-overlays/tdlib`. The overlay pins TDLib to an exact upstream commit and
archive SHA512, and `vcpkg.json` overrides `tdlib` to the overlay version.

The overlay is updated with `scripts/update_tdlib_overlay.sh`, which resolves
the requested TDLib commit, reads the upstream project version, computes the
archive SHA512, and updates the overlay plus manifest override in one step.

## Consequences

### Positive

- CI can validate the real Telegram gateway against the newest TDLib API before
  deployment.
- Builds remain reproducible: there is no floating `master` checkout at
  configure, build, or runtime.
- Docker, local builds, and GitHub Actions all resolve the same TDLib source.
- TDLib-specific fixes, such as cross-compilation generator handling, live with
  the TDLib port instead of leaking into project CMake.

### Negative

- The project now owns a small amount of vcpkg port maintenance.
- A TDLib bump invalidates the binary cache and can make the first CI run slow.
- Overlay patches must be reviewed carefully because they run during dependency
  builds on every supported platform.

### Neutral

- The vcpkg registry baseline still controls Boost, OpenSSL, fmt, spdlog,
  SQLite, Catch2, RapidCheck, and the rest of the dependency graph.
- If the public vcpkg registry catches up, keeping the overlay is still useful
  when CMLB needs an upstream commit newer than the packaged port.

## Alternatives Considered

### Option A: Wait for the public vcpkg tdlib port

Pros: no local port maintenance and less dependency churn.

Cons: the real Telegram gateway cannot use or compile-check new TDLib APIs until
the registry updates. That creates a deployment blind spot for Telegram API
changes.

Rejected because CMLB explicitly keeps the real TDLib build in CI to catch API
drift early.

### Option B: Build TDLib from a floating Git branch

Pros: always consumes the newest upstream commit.

Cons: not reproducible. Two builds from the same CMLB commit can produce
different binaries, and a transient upstream break can fail deployments without
a reviewed CMLB change.

Rejected because package resolution must be pinned, not floating.

### Option C: Vendor TDLib as a submodule

Pros: exact source pin and full control over patches.

Cons: larger checkout, more manual build integration, weaker vcpkg binary-cache
reuse, and more cross-platform maintenance in project CMake.

Rejected because a vcpkg overlay gives the same source pin while preserving the
existing manifest-mode build.

## References

- Related ADRs: ADR-0003, ADR-0004, ADR-0012
- Related files: `vcpkg-overlays/tdlib/portfile.cmake`,
  `scripts/update_tdlib_overlay.sh`, `CMakeLists.txt`
