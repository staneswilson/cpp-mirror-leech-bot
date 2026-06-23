# ADR-0012: TDLib include isolation is enforced by static analysis and source probe

- **Status:** Accepted
- **Date:** 2026-06-23
- **Deciders:** Engineering team
- **Related:** [ADR-0003 Telegram gateway isolation](0003-telegram-gateway-isolation.md), [ADR-0010 Security boundary](0010-security-boundary-and-credential-handling.md)

## Context

ADR-0003 established the important rule: TDLib headers belong in exactly one
translation unit, `src/infrastructure/telegram/telegram_gateway.cpp`.
Originally, CI planned to enforce that through clang-tidy, IWYU, and CMake
target visibility.

The IWYU job was optional, label-gated, and not part of the required production
gate. Keeping that workflow code gave the appearance of stronger enforcement
while adding install and cache work that default CI did not need. We still need
the TDLib boundary to fail fast on every push and pull request.

## Decision

Required CI enforces TDLib include isolation through:

- a source probe in `.github/workflows/static_analysis.yml` that greps
  `include`, `src`, and `tests` for TDLib includes outside
  `src/infrastructure/telegram/telegram_gateway.cpp`;
- full clang-tidy on pushes and changed-source clang-tidy on pull requests;
- CMake target visibility, with TDLib linked privately to the Telegram
  infrastructure implementation.

The optional IWYU workflow job is removed. IWYU remains acceptable as a local
developer tool, but it is not advertised as a required production gate.

## Consequences

### Positive

- The TDLib boundary is checked on every required Static Analysis run.
- CI avoids a heavyweight optional job that was usually skipped.
- Failures are easy to understand: the source probe prints the offending include
  line before exiting.
- Documentation now matches the live workflow instead of a planned one.

### Negative

- The grep probe is intentionally narrower than IWYU. It catches direct TDLib
  includes, not every possible transitive include-cleanliness issue.
- Contributors who want full include hygiene still need to run IWYU locally.

### Neutral

- ADR-0003's architecture decision remains unchanged. This ADR updates the CI
  implementation of that decision, not the TDLib isolation boundary itself.

## Alternatives Considered

### Option A: Keep optional IWYU in CI

This preserves the original enforcement story but leaves a slow, mostly skipped
job in the production workflow set.

- **Pros:** Broader include-cleanliness diagnostics when enabled.
- **Cons:** Adds maintenance burden and does not protect default push/PR runs.
- **Rejected because:** default CI must be fast and honest about what it
  enforces.

### Option B: Rely only on CMake private target visibility

Remove source-level checks and let non-gateway TDLib includes fail when their
target cannot find TDLib headers.

- **Pros:** No extra workflow code.
- **Cons:** The error appears later and can be obscured by compiler diagnostics.
- **Rejected because:** a direct source probe fails earlier and explains the
  policy in the job log.

### Option C: Custom clang-tidy plugin

Build or vendor a project-specific clang-tidy check for forbidden includes.

- **Pros:** Precise diagnostics integrated into clang-tidy.
- **Cons:** Extra binary/toolchain maintenance for a rule that grep expresses
  directly.
- **Rejected because:** the enforcement rule is simple enough to keep as a
  shell probe.
