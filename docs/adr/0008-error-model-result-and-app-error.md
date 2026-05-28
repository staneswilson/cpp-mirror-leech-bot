# ADR-0008: Error Model — `Result<T>` + structured `AppError`

- **Status:** Accepted
- **Date:** 2026-05-21
- **Deciders:** Engineering team
- **Supersedes:** none
- **Related:** [ADR-0001 Async model](0001-async-model-asio-coroutines.md)

## Context

CMLB has three obligations the error path has to honour at once:

1. Every external I/O call can fail (network, disk, RPC peer, subprocess
   exit) and the application has to react — retry, fall back, cancel
   siblings, surface a friendly message to the chat — without exceptions
   leaking across coroutine resumption points (where they have historically
   been a source of debugger pain and TSan false positives).
2. Operators reading `logs/cmlb.log` need a stable, machine-parseable
   error code so alert rules can fire ("Aria2Rpc spiked 20× in 5 min")
   without grepping free-form English.
3. End users reading the chat message need a friendly description ("Network
   error") not a raw enumerator name (`ErrorCode::Network`).

Mainstream C++ options each fall short of one of these:

- **Exceptions** — invisible in signatures, transport across coroutine
  frames is well-defined but expensive on the throw path, and our hot
  loops touch dozens of awaitables per request. Also incompatible with
  the `[[nodiscard]] Result<T>` discipline we want for use cases.
- **`int` / `errno`-style returns** — no payload, no source location, no
  type-system enforcement that the caller checks the value.
- **`std::optional<T>` + side-channel diagnostics** — loses the *why*.
  Triage from the log alone becomes guesswork.

## Decision

CMLB ships a hand-rolled but tiny error type and a `std::expected`-based
alias:

```cpp
namespace cmlb::core {

enum class ErrorCode {
    None, InvalidArgument, InvalidConfiguration, InvalidState,
    NotFound, AlreadyExists, PermissionDenied, Unauthenticated,
    Cancelled, DeadlineExceeded, ResourceExhausted, QuotaExceeded,
    Network, Timeout, Io, FileSystem,
    Serialization, Deserialization, JsonParse,
    TelegramApi, Aria2Rpc, QbittorrentApi, GoogleDriveApi, RcloneInvocation,
    Database, Migration,
    SubprocessFailed, MediaProcessing, ArchiveProcessing,
    Internal, Unknown,
};

struct AppError {
    ErrorCode             code;
    std::string           message;
    std::source_location  where = std::source_location::current();
};

template <typename T>
using Result = std::expected<T, AppError>;

[[nodiscard]] inline AppError error(ErrorCode c, std::string m,
                                    std::source_location w =
                                        std::source_location::current()) {
    return AppError{c, std::move(m), w};
}

}  // namespace cmlb::core
```

Rules applied across the codebase:

- Every function that can fail returns `Result<T>` (or
  `awaitable<Result<T>>` for async). `[[nodiscard]]` is mandatory.
- The constructor of `AppError` captures `std::source_location` at the
  call site — every log line carries `file:line:function` for free.
- The error message is human-readable English with the *cause*, not the
  *operation*. Never `"add_uri failed: bad URL"` — that's the stage. Say
  `"URL is missing scheme"` and let the caller wrap it with the stage
  if it wants to.
- For chat surfaces we never leak the raw `ErrorCode` enumerator name.
  `cmlb::core::friendly_error_label(code)` returns the user-facing label
  (`"Network error"`, `"Timed out"`); the enumerator name is for logs
  only via `cmlb::core::error_code_name(code)`.
- Cancellation is an error (`ErrorCode::Cancelled`), not a magic empty
  result. It propagates the same way as any other failure but is
  rendered as `"Cancelled"` in chat rather than `"Failed (Cancelled)"`.
- No `throw` in our own code outside of the test harness and one-shot
  config validation. Third-party throws (Boost.Beast, libstdc++ at
  edges) are caught at the adapter boundary and converted.

## Consequences

### Positive

- Use-case bodies read as a stream of `auto x = co_await dep_.op(...); if
  (!x) co_return std::unexpected(x.error());` — a uniform shape across
  every external call. The dispatcher and the renderer only have to know
  the `AppError` type to do their jobs.
- Triage from `cmlb.log` is fast: every error line has the code name plus
  the source location of the originating `error(...)` call.
- The chat surface stays clean — `HtmlRenderer::render_error` /
  `render_failure` already use `friendly_error_label`, escape the
  message, and truncate it before composing `<b>Failed</b>` blocks.
- Exception-free hot paths satisfy `-fno-exceptions`-curious profiling
  experiments without code churn (we don't ship that flag, but the door
  is open).

### Negative

- Manual error propagation is more typing than `try { ... } catch`.
  Mitigated by an editor snippet and the uniform `co_return
  std::unexpected(x.error());` shape.
- `std::expected<void, AppError>` is `Result<void>` and requires
  `co_return Result<void>{};` on the success path. Some callers find
  this verbose; we accept it for symmetry.
- We pay one `std::string` allocation per error. Profiling shows this is
  negligible compared with the I/O that the error is reporting on.

## Alternatives considered

1. **Bare `std::expected<T, ErrorCode>`.** Rejected: loses the message
   and source location, and chat copy would have to be reconstructed
   from the code alone.
2. **`std::error_code`.** Rejected: category boilerplate, no
   `source_location`, and the chat-friendly label still has to live
   elsewhere.
3. **Exceptions across coroutines.** Rejected: throw-cost on the hot
   path, no type-system signal at the call site, harder to reason about
   with cancellation and structured concurrency.
4. **`outcome::result` (Boost.Outcome).** Rejected: a heavier dependency
   for a marginal feature set above what `std::expected` already gives
   us in C++23.

## Implementation notes

- `include/cmlb/core/error.hpp` defines `ErrorCode`, `AppError`,
  `Result<T>`, the `error(...)` helper, and `error_code_name(code)` for
  logs.
- `include/cmlb/core/formatting.hpp` defines `friendly_error_label(code)`
  — the only place chat-facing strings for error codes live.
- `src/application/mirror_url.cpp` and `src/application/leech_url.cpp`
  use the `fail_task(stage, AppError)` /
  `fail_with(stage, ErrorCode, message)` pattern: the use case always
  has both the *stage* it was in and the *cause* it's reporting,
  formatted into a structured `<b>Failed</b>` block by the renderer.
- `src/presentation/callback_dispatcher.cpp` translates inline-button
  failures into toast alerts via `friendly_error_label`.
