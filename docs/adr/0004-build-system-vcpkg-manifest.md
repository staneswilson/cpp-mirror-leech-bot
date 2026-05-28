# ADR-0004: Build system — CMake + vcpkg manifest mode

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

CMLB depends on a sizeable C++ ecosystem: TDLib, Boost (Asio, Beast, JSON, Process), spdlog, fmt, sqlite-modern-cpp, SQLite itself, OpenSSL, zlib, libarchive, Catch2, RapidCheck, nlohmann_json (for non-Boost JSON paths if needed), and a handful of smaller libraries. Most of these have CMake config packages; a few do not. Most have stable releases; some pin to a recent commit.

We need a dependency management story that:

1. **Pins versions reproducibly.** A fresh clone on a fresh machine produces the same binary as CI. No `system openssl` surprises.
2. **Caches builds.** TDLib alone takes 15-30 minutes to compile. Doing that on every CI run would dominate the budget.
3. **Works across four toolchains.** Linux GCC, Linux Clang, MSVC, Apple Clang must all consume the same manifest with platform-specific overrides where genuinely needed.
4. **Plays well with CMake.** The project uses CMake; the dependency tool's output should be `find_package` discoverable without bespoke shim code.
5. **Survives a long project lifetime.** Dependency tooling changes over years; we want a tool that has institutional momentum.

The candidates were:

- vcpkg in classic mode (global package registry installed by `vcpkg install ...`).
- vcpkg in manifest mode (a `vcpkg.json` checked into the repo).
- Conan 2.x.
- Pure system packages (apt / brew / vcpkg-on-Windows for portability).
- Git submodules of each dependency, vendored in `third_party/`.

## Decision

CMLB uses **vcpkg in manifest mode**. The repository contains `vcpkg.json` (the manifest), `vcpkg-configuration.json` (the registry pinning, including a baseline commit), and a CMake toolchain file (`cmake/vcpkg.cmake`) that wires `find_package` into vcpkg's exports.

The manifest lists every direct dependency by name with feature flags where applicable:

```jsonc
{
  "name": "cmlb",
  "version-string": "0.1.0-alpha",
  "dependencies": [
    { "name": "tdlib", "version>=": "1.8.30" },
    { "name": "boost-asio", "version>=": "1.85.0" },
    { "name": "boost-beast", "version>=": "1.85.0" },
    { "name": "boost-json",  "version>=": "1.85.0" },
    { "name": "boost-process","version>=": "1.85.0" },
    { "name": "sqlite3", "features": ["json1", "fts5"] },
    "sqlite-modern-cpp",
    "spdlog",
    "fmt",
    "openssl",
    "zlib",
    "libarchive",
    { "name": "catch2", "version>=": "3.5.0" },
    "rapidcheck"
  ]
}
```

`vcpkg-configuration.json` pins the vcpkg registry to a specific baseline commit, locking all transitive versions.

CMake presets (`CMakePresets.json`) reference the vcpkg toolchain file:

```jsonc
{
  "cacheVariables": {
    "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  }
}
```

CI uses vcpkg's binary caching backed by GitHub Actions cache (or a self-hosted NuGet feed). A clean CI run with a warm cache restores TDLib and Boost in under a minute. A clean CI run with a cold cache (e.g. a baseline bump) takes the full TDLib compile.

## Consequences

### Positive

- **Reproducibility is real.** The baseline commit in `vcpkg-configuration.json` plus the manifest fully specify every transitive version. Two developers on different platforms get the same binaries (modulo platform-specific code paths).
- **Binary caching saves real time.** TDLib's 20-minute compile happens once per baseline. After that, every CI run, every developer build, every Docker image build pulls a cached binary.
- **Manifest is declarative and reviewable.** `vcpkg.json` is the single source of truth for what we depend on. A dependency bump is a one-file PR.
- **CMake integration is first-class.** `find_package(spdlog CONFIG REQUIRED)` works without bespoke shims because vcpkg installs config packages.
- **Cross-platform support is uniform.** Linux, macOS, and Windows consume the same manifest. Platform-specific patches live in vcpkg's port files (which we rarely need to touch).
- **Long-term momentum.** vcpkg is maintained by Microsoft, used at scale by Visual Studio's C++ workloads, and has a large port library. It is unlikely to be abandoned in our project's lifetime.

### Negative

- **vcpkg's port catalogue isn't always at the latest upstream version.** Occasionally we want a version that vcpkg hasn't packaged yet. The mitigation is the overlay-port mechanism (`vcpkg/ports/<name>/` checked into the repo); we keep this for emergencies and try not to use it.
- **First-time build is slow.** A developer on a fresh machine waits for TDLib (and Boost, and OpenSSL) to compile from source before they see a CMLB binary. The binary cache helps everyone except the first builder.
- **Binary cache needs configuration.** CI needs to wire up the GitHub Actions cache or a NuGet feed. The first time CI ran this it took an afternoon to debug. After that it is stable.
- **vcpkg's port system is C++ centric.** That is also a "Positive" entry above; it depends on perspective. We don't ship Python; we don't need a tool that handles Python.

### Neutral

- A `VCPKG_ROOT` environment variable is required. We document this in `runbook.md` and `CONTRIBUTING.md`. The CMake presets fail with a clear message if it isn't set.
- Manifest mode is more recent than classic mode; some StackOverflow answers reference classic mode. New contributors occasionally have to be told "ignore the `vcpkg install <name>` instructions you found online".

## Alternatives Considered

### Option A: vcpkg in classic mode

Run `vcpkg install <names>` once per developer machine; let the build pick up whatever is installed.

- **Pros:** Slightly simpler conceptually; fewer files in the repo.
- **Cons:** Versions drift between developers. Two contributors with different `vcpkg` clones get different binaries. There is no per-project pinning. CI must reproduce the right install state via a script that lives outside the repo.
- **Rejected because:** the entire point of dependency management is reproducibility.

### Option B: Conan 2.x

A Python-based C++ package manager with a similar feature set: manifest (`conanfile.py` or `conanfile.txt`), binary caching, version pinning.

- **Pros:** Mature; large package catalogue; flexible Python-based recipes; first-class CMake integration via `CMakeDeps` and `CMakeToolchain` generators.
- **Cons:** Requires Python on every build host (vcpkg requires only a C++ compiler and CMake). Conan 1 → Conan 2 migration was painful and split the ecosystem; the long-term direction of the project has been more turbulent than vcpkg's. The recipe ecosystem is community-maintained, which has both upsides and downsides (rapid updates, occasional gaps).
- **Rejected because:** vcpkg's tighter integration with the CMake / MSVC world matches our toolchain matrix better. The Python dependency for the build host is a real cost on tightly-locked-down build environments.

### Option C: System packages only

Use whatever `apt`, `brew`, and the system's package manager offer.

- **Pros:** Zero new tooling. Familiar to every Linux developer.
- **Cons:** Version drift between distros (Ubuntu 22.04 vs 24.04 ship different Boost, OpenSSL, SQLite versions). No story for macOS or Windows. TDLib is generally not packaged; we'd have to build it from source out-of-tree. Reproducibility is non-existent.
- **Rejected because:** a project that pins zero versions and uses whatever the host has is not buildable across our target matrix.

### Option D: Git submodules of each dependency, vendored in-tree

Add every dependency as a git submodule under `third_party/<name>/` and build it as part of the CMake graph (via `add_subdirectory`).

- **Pros:** Maximum control; no external tooling; offline-buildable.
- **Cons:** Multi-gigabyte clone. Updating a dependency is a multi-step ritual (`git submodule update --remote`, deal with whatever its build system needs). Boost alone is ~500 MB. There is no binary cache; every build recompiles everything. CMake `add_subdirectory` of large dependencies is fragile — they leak target names, override compiler flags, fight over `WARNING_FLAGS`.
- **Rejected because:** the maintenance burden is enormous and the build times are unworkable.

---

The decision is reviewable if (a) vcpkg stops being maintained, or (b) a transitive dependency demands a feature that vcpkg cannot accommodate via overlay ports. Neither risk is plausible in the project's foreseeable lifetime.
