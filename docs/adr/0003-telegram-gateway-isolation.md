# ADR-0003: TelegramGateway is the only file that includes TDLib headers

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

`<td/telegram/td_api.h>` is the public header of TDLib, the official Telegram client library. It declares approximately 1,500 generated message-class types (one per API method and one per update). Including it has three concrete consequences:

1. **Compile-time cost.** The header pulls in tens of thousands of lines of templated code. A translation unit that does nothing but `#include <td/telegram/td_api.h>` and a `main()` compiles for several seconds on a modern machine.
2. **Header viral spread.** If a CMLB header exposes `td::td_api::object_ptr<foo>`, every translation unit that transitively includes that CMLB header pays the cost above. There is no way to forward-declare `td_api::*` types meaningfully because they are template instantiations.
3. **Conceptual coupling.** Once TDLib types leak past the gateway, business logic starts referencing them. A `Task` that has a `td::td_api::object_ptr<message>` field cannot be tested without a TDLib client. The domain layer's purity guarantee dissolves.

We need TDLib (it is the only sane way to talk to Telegram from C++ — the HTTP Bot API is too limited and forbids files larger than 50 MB). What we need to avoid is letting TDLib become a transitive dependency of every file in the project.

## Decision

Exactly **one source file** in the entire codebase is allowed to include `<td/telegram/td_api.h>`: `src/infrastructure/telegram_gateway.cpp`.

The gateway:

1. Owns the `td::Client` instance.
2. Drives TDLib's update loop on a dedicated `asio::strand`.
3. Converts every TDLib update object into CMLB's `IncomingUpdate` discriminated-union type, which is defined in CMLB headers only.
4. Converts every CMLB outgoing request (`SendText`, `EditText`, `SendDocument`, `DownloadFile`, ...) into a TDLib API call.

The gateway's public header (`include/infrastructure/telegram_gateway.hpp`) does **not** include `<td/telegram/td_api.h>`. It declares the gateway class with a forward-declared `Impl` (PIMPL), and uses only CMLB-native types in its public API.

Enforcement is multi-layered:

- **CMake target visibility.** TDLib is linked `PRIVATE` to a single target — `cmlb_infrastructure_telegram` — that contains exactly `telegram_gateway.cpp`. Any other target attempting `#include <td/telegram/...>` fails at the preprocessor stage because the include path is not propagated.
- **clang-tidy header-filter.** A project-level `.clang-tidy` rule (`misc-include-cleaner` plus a custom `forbidden-include` rule from our `tools/clang-tidy/`) blocks `<td/telegram/td_api.h>` outside the allowed file. CI runs clang-tidy with `-warnings-as-errors=*`.
- **include-what-you-use.** A separate CI job runs IWYU over the whole project with a mapping file that lists `<td/telegram/td_api.h>` as forbidden except in `telegram_gateway.cpp`. IWYU's output is parsed and any new violation fails the job.
- **Pre-merge code review.** The PR template asks reviewers to check for new TDLib includes outside the gateway. This is the human backstop.

## Consequences

### Positive

- **Compile times stay manageable.** A change to a use case or to a domain type rebuilds a handful of translation units. A change to `telegram_gateway.cpp` rebuilds *only* that translation unit; the rest of the project sees a stable header surface.
- **Use cases are testable without TDLib.** `MirrorUrl` accepts a `Messenger&` interface. Tests inject a `FakeMessenger` and never link TDLib. Catch2 test binaries that don't need TDLib build in seconds.
- **The Telegram dependency is replaceable in principle.** If TDLib ever becomes unmaintained, or if a future requirement makes the HTTP Bot API viable, we replace one file. The application layer doesn't change.
- **The Telegram-specific bugs are localised.** TDLib has its own concurrency model and its own quirks (update ordering, file id lifetimes, parse-mode edge cases). All of that knowledge lives in one file. New contributors learn TDLib if and only if they touch that file.
- **The build graph is honest.** It is immediately clear from `CMakeLists.txt` that exactly one target depends on TDLib. Auditors can confirm this in seconds.

### Negative

- **PIMPL has a small runtime cost.** A virtual call (or a pointer indirection through `std::unique_ptr<Impl>`) per gateway operation. Negligible compared to the TDLib RPC round-trip itself.
- **Translating TDLib updates is duplicated work.** Every TDLib update type has a hand-written translation into a CMLB type. About 30 update types are relevant; the translation table is ~500 lines. New TDLib types must be added by hand. This is a known cost; we treat the translation table as a deliberate API boundary, not as boilerplate.
- **The enforcement story has three independent tools.** clang-tidy, IWYU, and CMake target visibility all need to agree. If one of them fails (e.g. IWYU isn't available on a contributor's machine) the others still catch the violation, but onboarding is slightly more involved.

### Neutral

- The PIMPL pattern is a standard C++ idiom. Engineers familiar with the codebase don't experience it as friction.
- The translation table benefits from being one file: cross-references, search, and grep all just work.

## Alternatives Considered

### Option A: PIMPL only, no enforcement

Use `unique_ptr<Impl>` in the gateway header to keep TDLib out of *that* header, but don't enforce a project-wide ban on the include.

- **Pros:** Simpler. Same compile-time wins as the chosen option.
- **Cons:** Nothing stops a future contributor from adding `#include <td/telegram/td_api.h>` in a use case, "just this once". Without enforcement, the discipline rots. We've seen this happen on every C++ codebase that didn't have a tool blocking the bad case.
- **Rejected because:** the value of the boundary is that it is *guaranteed*, not merely *intended*. Without enforcement, the cost of policing is paid in code review forever; with enforcement, it's paid once in CI configuration.

### Option B: Namespace alias for the TDLib types

Re-export TDLib types from a CMLB namespace alias and let the rest of the codebase use the alias.

- **Pros:** Easy to introduce; minor renaming.
- **Cons:** Doesn't solve any of the problems. The alias still requires `<td/telegram/td_api.h>` to be in scope at every use site. Compile times are unchanged; conceptual coupling is unchanged; testability is unchanged.
- **Rejected because:** it is a cosmetic change that delivers none of the actual goals.

### Option C: Include TDLib everywhere; no boundary

The naive default.

- **Pros:** No code to write; no enforcement to set up; no PIMPL indirection.
- **Cons:** Compile times explode. Domain types become entangled with TDLib's generated types. Tests must link TDLib. The codebase becomes hostile to anyone who hasn't memorised the TDLib API.
- **Rejected because:** this is exactly the architecture we are rebuilding *away* from. The legacy CMLB had it; the new build does not.

---

The enforcement layers are coupled. If any one tool is removed (e.g. IWYU on macOS isn't available in the same versions as Linux) we still have the other two. The CMake target visibility alone would be sufficient in practice; clang-tidy and IWYU exist to surface violations *earlier*, before someone has spent time writing code that won't link.
