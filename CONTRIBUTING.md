# Contributing to CMLB

Thanks for your interest in CMLB. This document captures the conventions every
contribution -- from a typo fix to a new infrastructure adapter -- is expected
to follow. Read it once; refer back to the checklists when you raise a pull
request.

CMLB is a C++23 Telegram Mirror/Leech bot built around a five-layer
domain-driven architecture (`core`, `domain`, `application`, `infrastructure`,
`presentation`). The goal of these guidelines is to keep the codebase mergeable
in small, reviewable slices while preserving its production quality bar.

---

## Code of conduct

This project adopts the
[Contributor Covenant v2.1](./CODE_OF_CONDUCT.md). Participation in any project
forum -- issues, pull requests, discussions, or chat -- is governed by it.
Report unacceptable behaviour to the maintainers at the email listed in
`CODE_OF_CONDUCT.md`.

---

## How to file an issue

Before opening a new issue, please:

1.  Search [open](../../issues) and [closed](../../issues?q=is%3Aissue+is%3Aclosed)
    issues to avoid duplicates.
2.  Choose the right template:
    -   **Bug report** for unexpected behaviour, crashes, or regressions.
    -   **Feature request** for new capabilities or workflow improvements.
3.  Fill in every required section. Logs and reproduction steps are the most
    valuable inputs we receive -- redact bot tokens, chat IDs, and any
    personally identifying information.
4.  Tag the issue with the relevant layer label (`area/core`, `area/domain`,
    `area/application`, `area/infrastructure`, `area/presentation`) if you can.

Security-sensitive reports must follow the disclosure flow described in
[SECURITY.md](./SECURITY.md) -- do not file them in the public issue tracker.

---

## How to propose changes

1.  Open (or comment on) an issue to align on the approach before you start
    coding. Large changes without a design conversation are likely to be
    rejected.
2.  For non-trivial work, draft an Architecture Decision Record under
    `docs/adr/` using the template in `docs/adr/0000-template.md`. The ADR
    should be merged with -- or before -- the implementation.
3.  Fork the repository and create a short-lived branch (see
    [branching strategy](#branching-strategy)).
4.  Implement the change, keeping commits focused and reviewable.
5.  Open a pull request against `main` using the
    [pull request template](.github/PULL_REQUEST_TEMPLATE.md).

---

## Development setup

The end-to-end developer setup, prerequisites, and common workflows live in
[docs/runbook.md](./docs/runbook.md). At a glance:

-   **Toolchain:** C++23 with the project matrix: GCC 14, Clang 20, MSVC 2022,
    or Apple Clang on the current macOS runner.
-   **Build system:** CMake 3.28+ via presets (`cmake --preset debug`).
-   **Dependencies:** vcpkg manifest mode. The registry baseline is pinned in
    both `vcpkg.json` and `vcpkg-configuration.json`; do not use floating
    dependency versions in build or deployment scripts.
-   **Pre-commit:** `pip install pre-commit cmakelang commitizen` then
    `pre-commit install --install-hooks` from the repo root.
-   **Tests:** `ctest --preset debug --output-on-failure`.

See the runbook for platform-specific notes (Windows TDLib build, WSL caveats,
Sanitizer presets, coverage runs).

---

## Branching strategy

We follow **trunk-based development** with short-lived feature branches.

-   `main` is always green and deployable. Direct pushes are disallowed.
-   Feature branches are created from `main` and rebased onto `main` before
    merge. Squash-merge is the default; preserve a meaningful commit message.
-   Branches should live for **no more than a few days**. Split larger work
    behind feature flags rather than long-running branches.

### Branch naming

| Prefix     | Purpose                                           | Example                            |
| ---------- | ------------------------------------------------- | ---------------------------------- |
| `feat/`    | New user-visible capability                       | `feat/aria2-progress-stream`       |
| `fix/`     | Bug fix that does not change the API              | `fix/upload-retry-timeout`         |
| `chore/`   | Tooling, refactors with no behaviour change       | `chore/bump-vcpkg-baseline`        |
| `docs/`    | Documentation-only changes                        | `docs/runbook-wsl-section`         |
| `test/`    | Tests-only changes                                | `test/router-property-suite`       |
| `perf/`    | Performance work without functional change        | `perf/asio-buffer-pooling`         |
| `refactor/`| Code restructuring with no behaviour change       | `refactor/extract-status-renderer` |

Slugs are kebab-case, descriptive, and **scoped** -- prefer
`feat/qbittorrent-rpc-client` over `feat/torrents`.

---

## Conventional Commits

CMLB enforces [Conventional Commits 1.0](https://www.conventionalcommits.org/)
via `commitizen` (configured in `.pre-commit-config.yaml`). Each commit message
must follow:

```
<type>(<optional-scope>): <imperative summary, <= 72 chars>

<body explaining *why*, wrapped at 72 columns>

<optional footers, e.g. BREAKING CHANGE: ..., Refs: #123>
```

### Allowed types

| Type       | Use for                                                                |
| ---------- | ---------------------------------------------------------------------- |
| `feat`     | A new feature visible to users or other layers.                        |
| `fix`      | A bug fix.                                                             |
| `docs`     | Documentation-only changes.                                            |
| `style`    | Formatting changes that do not affect meaning (whitespace, semicolons).|
| `refactor` | Code change that neither fixes a bug nor adds a feature.               |
| `perf`     | Performance improvement.                                               |
| `test`     | Adding or correcting tests.                                            |
| `build`    | Changes to build system, CMake, vcpkg manifest.                        |
| `ci`       | Changes to CI configuration or scripts.                                |
| `chore`    | Maintenance tasks (deps bump, generated files, housekeeping).          |

### Examples

```text
feat(application): add /clone command with quota enforcement

fix(infrastructure/telegram): retry on TDLib 429 with exponential backoff

docs(adr): record decision to adopt Boost.Asio coroutines

refactor(domain): split download_request value object from aggregate

perf(uploader): reuse OpenSSL contexts across chunk uploads

build: pin vcpkg baseline to 2025-04-12

ci(release): publish SBOM alongside the binary artefact

chore: bump spdlog to 1.14.1

feat(api)!: change OnProgress signature to return awaitable<void>

BREAKING CHANGE: callers must await the returned task; sync invocation
is no longer supported.
```

Breaking changes are denoted by a `!` after the type/scope **and** a
`BREAKING CHANGE:` footer. The footer is mandatory.

---

## Coding standards

-   `#pragma once` only; no include guards.
-   File names: `snake_case.hpp` / `snake_case.cpp`.
-   Classes: `PascalCase`. Free functions and methods: `snake_case`.
-   Member fields end with `_` (e.g. `client_`, `retry_count_`).
-   Interfaces live in `xxx_interface.hpp` and expose `class XxxInterface` --
    we do not use an `I` prefix.
-   Do not introduce `_utils.hpp` / `_helpers.hpp` / `Manager`-suffixed files.
    If you reach for one of those, the seam is wrong.
-   4-space indent, 100-column lines. `.clang-format` enforces both.
-   Async code uses Boost.Asio coroutines and returns
    `awaitable<Result<T>>`. Avoid raw callbacks.
-   Database access uses `sqlite-modern-cpp` against a SQLite database opened
    in WAL mode -- never block the IO thread on a query.
-   Logging is structured via spdlog; never use `std::cout` / `std::cerr` in
    library code.

`clang-tidy` is run with `WarningsAsErrors: '*'`. Address findings; do not
suppress them without an inline `// NOLINT(check-name) // reason` comment.

---

## Testing

-   Unit tests use Catch2 v3 with RapidCheck property-based suites where the
    domain admits invariants (e.g. parser round-trips, idempotent commands).
-   Place tests next to their target layer under `tests/<layer>/`.
-   Cover the happy path **and** at least one failure path per public entry
    point.
-   Long-running or integration tests must be tagged `[integration]` and gated
    behind the `CMLB_RUN_INTEGRATION_TESTS` ctest label.
-   Run `ctest --preset debug --output-on-failure` before pushing.

---

## Pull request checklist

Before requesting review, confirm that:

-   [ ] `clang-format` runs clean (`pre-commit run clang-format --all-files`).
-   [ ] `clang-tidy` runs clean (`scripts/run_static_analysis.sh` after
        `cmake --preset debug`).
-   [ ] New code is covered by tests; existing tests still pass.
-   [ ] An ADR exists for any non-trivial design decision.
-   [ ] `CHANGELOG.md` has an entry under `## [Unreleased]` describing the
        change in user-facing terms.
-   [ ] Documentation in `docs/` is updated when behaviour changes.
-   [ ] Commit messages follow Conventional Commits.
-   [ ] No secrets, tokens, or personally identifying information are in the
        diff.
-   [ ] The PR description fills in every section of the template.

PRs that fail these checks will be sent back without review.

---

## Code review expectations

-   The author is responsible for the change until it merges -- keep watch on
    notifications, address feedback promptly, and rebase on `main` as needed.
-   Reviewers acknowledge a PR within **one business day** and provide a full
    review within **24 hours** of receiving review-ready status.
-   Infrastructure changes (anything under
    `src/infrastructure/`, `include/cmlb/infrastructure/`, CI, build,
    deployment) require **two approvals**, one of whom must be a project
    maintainer. All other changes require **one approval**.
-   Use suggestion blocks for small fixes; raise discussions in conversation
    threads. Resolve threads once addressed; never resolve another reviewer's
    thread on their behalf.
-   Prefer "nit:" prefixes for non-blocking style observations so authors can
    triage feedback quickly.
-   `Approved` is a signal that you have read the diff, run the build locally
    when relevant, and would be comfortable owning the change. It is not a
    rubber stamp.

---

## Release process

-   CMLB uses [Semantic Versioning 2.0](https://semver.org/):
    -   `MAJOR` -- incompatible API or behaviour change.
    -   `MINOR` -- backwards-compatible feature addition.
    -   `PATCH` -- backwards-compatible bug fix.
-   The current canonical version lives in [`VERSION`](./VERSION) and is
    mirrored into the CMake project version.
-   Releases are cut by:
    1.  Promoting the `## [Unreleased]` block in `CHANGELOG.md` to a versioned
        section with the release date.
    2.  Bumping `VERSION` and `vcpkg.json`.
    3.  Tagging the release commit: `git tag -s vX.Y.Z -m "vX.Y.Z"`.
    4.  Pushing the tag, which triggers `.github/workflows/release.yml` to
        build, sign, and publish binaries plus an SBOM.
-   Hotfix patch releases are cut from a `release/X.Y` branch when `main` has
    diverged with unrelated work.

---

## Getting help

-   Open a draft pull request to discuss approach with code attached.
-   Join the project chat (link in the repository description).
-   Tag `@maintainers` in an issue when you need triage.

Thanks again for contributing. Quality work compounds -- every clean diff
makes the next one easier to land.
