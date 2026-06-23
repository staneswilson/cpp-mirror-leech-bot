# ADR-0005: Warnings-as-errors, symmetric across all four toolchains

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

The CMLB CI matrix compiles the project on four toolchains:

- Linux GCC 14.
- Linux Clang 20 (with separate ASan, UBSan, TSan jobs).
- Windows MSVC 19.38 (Visual Studio 2022 17.8+).
- macOS Apple Clang 15.

The legacy CMLB CMake configuration had a pattern that is common in C++ projects but corrosive over time: warnings were enforced strictly on one toolchain (MSVC, with `/W4 /WX`) and explicitly commented out on the others (`# add_compile_options(-Wall -Wextra -Werror) # disabled: too noisy on GCC`). The result was a codebase where MSVC builds were clean, GCC builds had hundreds of warnings, and a release shipped if MSVC was green even when GCC was screaming.

The asymmetry has predictable consequences:

- Engineers fix warnings only when they break the strict toolchain. The lax toolchains accumulate technical debt.
- Bugs that one compiler catches (`-Wreturn-type`, `-Wuninitialized`, MSVC's C26495) are filtered out by the lax compilers' silence.
- Engineers learn to ignore warnings as background noise on the lax toolchains. New warnings get lost in the existing pile.
- Cross-platform parity erodes: code that compiles on MSVC might fail on GCC for trivial reasons that should have been caught at the warning level.

We need a policy that closes this gap.

## Decision

All four toolchains compile with **warnings-as-errors** at a strict warning level, in CI and locally. The warning sets are chosen to be **as symmetric as feasible** across compilers; where a warning is genuinely platform-specific or buggy on one compiler, the suppression is opt-in per file via a `#pragma` and accompanied by a comment justifying it.

The flag sets:

- **GCC and Clang:** `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough` plus Clang-specific `-Wmove-on-temporary` and `-Wunused-but-set-variable`.
- **MSVC:** `/W4 /WX /permissive- /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296 /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928 /w15038 /Zc:__cplusplus /Zc:preprocessor`.
- **Apple Clang:** same as Linux Clang.

`CMakePresets.json` defines these per-toolchain so a developer doesn't need to remember them. CI uses the same presets — local and CI behaviour is identical.

A small set of warnings is **suppressed globally** because they fire on third-party headers (Boost, TDLib) that we cannot fix and do not want to wrap. Suppressions are localised to the include of the relevant header via:

```cpp
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <td/telegram/td_api.h>
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
```

Per-file suppressions inside our own code are reviewed individually. The pattern is to suppress around the smallest possible scope and to add a comment explaining why.

## Consequences

### Positive

- **Bugs surface earlier.** `-Wuninitialized`, `-Wnull-dereference`, MSVC's `C26495` series — these catch real issues. Catching them at compile time on every commit is cheaper than at runtime in production.
- **Cross-compiler parity stays intact.** A change that introduces a GCC warning fails CI on the GCC job. The author cannot merge until it's fixed. The codebase stays buildable everywhere.
- **No "ignore the noise" culture.** Because warnings are errors, there is no noise to ignore. The build is green or it is broken.
- **Code review is cheaper.** Reviewers don't have to spot constructs that "would warn on GCC" — the GCC job has already enforced it.
- **Onboarding signals quality.** A new contributor cloning the repo and running `cmake --preset debug && cmake --build --preset debug` gets a clean build with no warnings. That is a deliberate first impression.

### Negative

- **Initial cleanup cost.** Bringing an existing codebase up to this standard is a multi-day effort. CMLB pays this cost during the rewrite, not afterwards.
- **Some warnings are pedantic.** `-Wsign-conversion` and `-Wconversion` flag legitimate constructs. The fix is usually a narrow `static_cast<>` with a comment. This is occasionally annoying; we accept the annoyance because the warning catches *real* bugs in a fraction of cases.
- **Third-party headers force suppressions.** Boost and TDLib trip our flags. We wrap their includes; this is a small but real piece of plumbing.
- **A new compiler version can break the build.** Compiler upgrades introduce new warnings. We treat that as a deliberate signal — review the new warnings, fix or suppress with justification — rather than as breakage to roll back.

### Neutral

- The flag sets are not 100% symmetric (some MSVC warnings have no GCC equivalent and vice versa). The goal is "as symmetric as feasible", not "literally identical".
- We do not enforce warnings-as-errors on Release builds with optimisation-induced warnings (e.g. `-Wmaybe-uninitialized` is famously over-eager at high optimisation levels). The flag `-Wno-maybe-uninitialized` is applied to Release builds only, and reviewed periodically.

## Alternatives Considered

### Option A: Legacy asymmetric policy (MSVC strict, GCC commented-out)

The starting point.

- **Pros:** Easy to maintain. New code compiles on the lax toolchains.
- **Cons:** Every problem in the [Context](#context) section.
- **Rejected because:** the policy actively erodes code quality and cross-platform parity.

### Option B: Symmetric warnings-as-errors except on macOS Apple Clang

Some projects ship Apple Clang at a more lenient level because Apple Clang trails upstream Clang by 1-2 versions.

- **Pros:** Slightly reduced friction for macOS-only contributors.
- **Cons:** Reintroduces asymmetry. macOS developers ship code that breaks the Linux Clang job. The "feels familiar but isn't the same" failure mode is worse than either strict-everywhere or lax-everywhere.
- **Rejected because:** the asymmetry will inevitably bite. Apple Clang trailing upstream by a version means a smaller set of available diagnostics, but the ones available match a subset of Linux Clang — so identical flags work; we just get slightly fewer diagnostics on Apple. That's fine.

### Option C: Warnings as warnings, not errors

The opposite extreme: enable strict warnings but don't turn them into errors. Track them in CI as a metric.

- **Pros:** Easier ramp-up. No CI flakes from compiler version bumps.
- **Cons:** "Track as a metric" is a polite way of saying "ignore". Without enforcement, the count grows monotonically until the metric becomes meaningless.
- **Rejected because:** every project that has tried this has watched its warning count grow into the thousands.

---

The decision is non-negotiable for v1. If a specific warning is genuinely buggy on one compiler version, we document the suppression and revisit when the compiler is updated. The goal is a build that is green on all four toolchains, every commit, with no exceptions accumulated over time.
