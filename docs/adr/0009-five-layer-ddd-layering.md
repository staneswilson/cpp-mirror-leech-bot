# ADR-0009: Five-Layer DDD — Dependency Discipline

- **Status:** Accepted
- **Date:** 2026-05-21
- **Deciders:** Engineering team
- **Supersedes:** none
- **Related:** [ADR-0003 Telegram gateway isolation](0003-telegram-gateway-isolation.md), [ADR-0006 No `I`-prefix naming](0006-no-i-prefix-naming.md)

## Context

The original Python-style mirror-leech codebases share a recognisable
failure shape: a god-class (`BotEngine`, `Client`, `Worker`) owns every
external connection, every queue, and the chat-formatting logic, all at
once. Adding a feature means touching that one class. Adding a test means
mocking everything it touches. Removing a feature is impossible because
state from elsewhere has leaked into the same object.

CMLB is a rewrite, and the rewrite has to make the architecture
*enforceable*, not aspirational. The goal isn't a layered diagram in
`docs/`; it's a build that refuses to link if the layering breaks.

## Decision

CMLB ships as five concentric layers. Each layer is a separate CMake
static library. Dependencies point inwards only:

```
presentation  →  application  →  infrastructure  →  domain  →  core
```

Each layer's responsibility:

- **`core/`** — primitives. `Result<T>`, `AppError`, `Logger`,
  `Configuration`, `Executor`, `Cancellation`, `Formatting` (binary/SI
  byte units, progress bars, HTML escape, UTF-8-safe truncation,
  friendly error labels). No business logic. No external I/O.
- **`domain/`** — the business model. `Task` aggregate, `Authority`,
  `StrongId<Tag, Underlying>` (phantom typing — `ChatId` is not
  comparable with `UserId`), `ByteSize`, `UploadDestination`. Pure
  values and state machines. No I/O, no logger, no Asio.
- **`infrastructure/`** — every external adapter. TDLib gateway, aria2
  WebSocket JSON-RPC client, qBittorrent Web API client, Google Drive
  upload client, rclone subprocess wrapper, SQLite repositories,
  filesystem and process abstractions. Implements the
  `*_interface.hpp` contracts that downstream layers depend on.
- **`application/`** — use cases. One file per verb-noun (`mirror_url`,
  `leech_url`, `cancel_task`, ...) — each constructor-injected with
  the narrow infrastructure interfaces it needs. Each exposes a single
  `awaitable<Result<T>> execute(Request)` method. The orchestration
  layer; never reaches back into the messenger directly past
  `MessengerInterface`.
- **`presentation/`** — the user-facing surface. `CommandParser`,
  `CommandDispatcher`, `CallbackDispatcher`, `HtmlRenderer`,
  `ProgressRenderer`. Produces every chat message that leaves the bot
  (Telegram HTML subset only: `<b>`, `<i>`, `<code>`, `<pre>`). Maps
  inbound commands to use-case calls; never knows what an `aria2c`
  WebSocket looks like.

### Rules

1. **Header includes.** A `.hpp` in `include/cmlb/<L>/...` may only
   include from layers it depends on or itself. Tested by clang-tidy
   and the standard library inclusion ordering convention.
2. **CMake link edges.** `target_link_libraries(... PUBLIC ...)` lists
   only the layers whose headers are reachable from the current
   layer's public headers. Implementation-only deps (Boost::system in
   a `.cpp`, fmt::fmt for body composition) go in `PRIVATE`.
3. **Interfaces own the contract.** Infrastructure exports
   `*_interface.hpp` headers (e.g. `DownloaderInterface`,
   `UploaderInterface`, `MessengerInterface`). Use cases hold
   references to interfaces, never to concrete classes. Tests inject
   in-memory fakes derived from the same interfaces.
4. **No upward includes.** `presentation/` may include from
   `application/`, `application/` may include from `infrastructure/`,
   etc. The reverse is a build error.
5. **No god-engine.** There is no `BotEngine`, `App`, or `Manager`
   class that aggregates every interface. The composition root
   (`src/main.cpp`) wires up infrastructure, hands references into
   use cases, and registers use cases into the dispatcher. After
   wiring, the composition root has nothing left to do.
6. **Naming.** Layer namespaces are `cmlb::core`, `cmlb::domain`,
   `cmlb::infrastructure::<area>`, `cmlb::application`,
   `cmlb::presentation`. Layer name appears in the namespace, the
   header path, and the source path — three places to spot a leak.

### Concrete consequence: `escape_html` lives in `core`, not `presentation`

A recent refactor needed `escape_html` and `truncate_for_display` for
HTML composition in `mirror_url.cpp` (application). Adding
`#include <cmlb/presentation/html_renderer.hpp>` to an application TU
would have inverted the dependency direction. Instead the three pure
string helpers (`escape_html`, `truncate_for_display`,
`friendly_error_label`) moved into `cmlb::core::formatting` where any
layer can consume them. `HtmlRenderer` retains thin static wrappers so
presentation-side callers don't churn. This is the layering rule in
action: when a helper looks like it wants to climb up a layer, the
helper is the part that's misplaced — push it down to where it
belongs.

## Consequences

### Positive

- A bug report that mentions "command parsing" lives entirely in
  `src/presentation/command_parser.cpp`. A bug report about "aria2
  reconnect" lives in `src/infrastructure/download/aria2_downloader.cpp`.
  Localised faults, localised fixes.
- Tests are cheap. Use-case tests inject in-memory fake repositories,
  fake messenger, fake downloader. No mocking framework — interfaces
  are small and hand-rolled fakes are dozens of lines.
- A new uploader (S3, Box, IPFS) means one new file in
  `src/infrastructure/upload/`, one entry in its `CMakeLists.txt`, one
  config struct extension, and one wire in `src/main.cpp`. The use
  cases don't change.
- Compilation is parallel. The five layer libraries are independent
  CMake targets; CI builds them concurrently.

### Negative

- More files. The "small, sharp tools" philosophy means every
  responsibility gets its own header — `download_options.hpp`,
  `download_status.hpp`, `downloader_interface.hpp` — instead of one
  fat `aria2.hpp`. Navigation is faster once the convention is
  internalised but the file tree looks daunting from outside.
- Interface boundaries cost performance via virtual dispatch. We
  measured this and it's noise on the upload/download hot path
  (network dominates by orders of magnitude). For genuinely hot
  in-process paths (`core/formatting`, `domain/byte_size`) we don't
  use interfaces — they're free functions and value types.
- Tooling needs to be on side. `clang-tidy.misc-include-cleaner` and
  IWYU catch most leak attempts; the strict CMake link edges catch
  the rest at link time.

## Alternatives considered

1. **Three layers (core / business / adapters).** Rejected: collapses
   the use-case layer into either the model or the adapters, which is
   exactly the god-class antipattern this ADR exists to prevent.
2. **Hexagonal architecture without explicit named layers.** Same
   semantics as what we have, but the lack of a layer namespace and
   library-per-layer makes leaks easier to overlook. We prefer the
   redundancy.
3. **One library, header-only modules.** Rejected: every change
   recompiles every TU. The build matrix (Linux/macOS/Windows × four
   compilers × four sanitiser presets) is unforgiving of full-tree
   rebuilds.

## Implementation notes

- `CMakeLists.txt` files: one per layer in `src/<layer>/`. Each lists
  exactly the sources it owns. `target_link_libraries` lines carry
  comments stating *why* each edge is `PUBLIC` (e.g.
  `cmlb_application` PUBLIC-links `cmlb_persistence` because
  `include/cmlb/application/cancel_task.hpp` takes a
  `TaskRepository&` in its constructor).
- `include/cmlb/<layer>/*.hpp` paths are reserved per layer; new files
  go into the matching directory or the build fails to find them.
- Naming: classes are `PascalCase`, free functions and methods are
  `snake_case`, members are `trailing_underscore_`. Interfaces use the
  `_interface.hpp` filename suffix and the `Interface` class suffix —
  there is no `I`-prefix (see ADR-0006).
