# ADR-0001: Async model — Boost.Asio C++20 coroutines

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

CMLB performs a large amount of concurrent I/O: it talks to Telegram via TDLib (single long-lived client, hundreds of in-flight requests), to aria2 via a WebSocket RPC, to qBittorrent via HTTPS, to Google Drive via HTTPS, to SQLite via a connection pool, and it spawns subprocesses for ffmpeg, 7z, and rclone. Each of these can take seconds to minutes, every one of them must be cancellable, and many run concurrently against the same set of resources (one TDLib client serving N users, one aria2 RPC connection multiplexed across M downloads).

The async model we pick has to satisfy:

1. **Composition.** Use cases like `MirrorUrl` chain a download, an extract, an upload, repository writes, and progress edits. The chain must read as straightforward code, not as a callback graveyard.
2. **Cancellation.** When a user issues `/cancel`, every in-flight step for that task — a TCP read, an HTTP request, a `sqlite3_step`, a `waitpid` on a subprocess — must abort within seconds, not at the next natural break.
3. **Backpressure.** A download producing progress events at 10 Hz cannot flood the message-edit pipeline (which Telegram caps near 1 Hz per chat). The model must let us await downstream consumers rather than queue without bound.
4. **Lifetime safety.** Long chains of asynchronous operations share state (the `Task`, the `cancellation_signal`, the destination message id). Aliasing this state across thread boundaries without a clear ownership story is the single biggest source of bugs in async C++ code.
5. **Toolchain support.** GCC 14, Clang 20, MSVC 19.38, and Apple Clang 15 must all compile the same async constructs cleanly with warnings-as-errors.

Four serious candidates were on the table.

## Decision

CMLB uses **Boost.Asio C++20 coroutines** as the sole async model. Every fallible I/O-performing function returns `asio::awaitable<Result<T>>`. Every long-running operation accepts an `asio::cancellation_slot`. The shared `asio::io_context` is owned by `core::Executor` and runs on a worker thread pool of size `executor.worker_threads`.

Concretely:

```cpp
asio::awaitable<Result<DownloadResult>>
Aria2Downloader::start(TaskId id, Source src, asio::cancellation_slot cs) {
    auto bind = asio::bind_cancellation_slot(cs, asio::use_awaitable);
    auto gid = co_await rpc_.call<std::string>("aria2.addUri", params(src), bind);
    if (!gid) co_return gid.error();
    // ... track progress via the RPC's notification stream ...
    co_return DownloadResult{ .path = ..., .bytes = ... };
}
```

Use cases compose these awaitables with plain `co_await`, propagate `Result<T>` errors via early-return, and never see raw Asio handlers.

## Consequences

### Positive

- **Linear-looking code.** Use cases read top-to-bottom. The control flow of `MirrorUrl::execute` looks like its sequence diagram. Reviewers can reason about it without simulating a state machine in their head.
- **First-class cancellation.** Asio's `cancellation_slot` propagates structurally. Wrapping a sub-awaitable with `bind_cancellation_slot` is one line. A cancelled coroutine cleans up its locals via normal destruction.
- **No invented runtime.** We rely on Boost.Asio, which is widely used, mature, and well-documented. The runtime cost is the cost of `io_context::run` on N threads. We do not maintain a custom scheduler, a custom task queue, or a custom cancellation propagation mechanism.
- **Backpressure is free.** A coroutine that awaits a downstream consumer (a strand-owned `Messenger::edit_text`) blocks naturally. There is no need for explicit channels with bounded capacity.
- **Composable with timers and signals.** `co_await asio::steady_timer{ex, 1s}.async_wait(use_awaitable)` is a single expression. Integrating cancellation, retries, and timeouts uses the same machinery.
- **Strands eliminate ad-hoc mutexes.** Resources that aren't thread-safe (TDLib client, an aria2 session) are wrapped in a strand. All access goes through it; no `std::mutex` needed.

### Negative

- **Coroutine compilation cost.** Heavy use of coroutine templates raises compile times. Mitigated by aggressive PCH and by keeping coroutine bodies in `.cpp` files behind a non-template public entry point where it matters.
- **Debugger experience varies by compiler.** GDB and LLDB handle coroutine frames but the developer ergonomics lag behind ordinary stack frames. MSVC's debugger is the best of the three; that's an accident of history, not something we can fix.
- **Iterator-style stream consumption is awkward.** An async generator from Asio is `experimental::coro<T>` which is still under the experimental namespace. CMLB uses notification-callback adapters (the RPC notification handler runs on a strand and pushes events into an `asio::channel`) to sidestep this.
- **No structured concurrency primitives in the standard sense.** `awaitable_operators::&&` and `||` exist in `boost::asio::experimental`, but they're not as polished as the structured-concurrency facilities being added to `std::execution`. CMLB uses them; if they regress we contain the fallout to a small wrapper module.
- **TDLib's thread model needs care.** TDLib delivers updates on its own thread. The gateway dispatches every update onto our strand before any business logic touches it. Forgetting that hop is a UB-class bug. The pattern is documented and enforced by code review.

### Neutral

- All four target compilers support C++20 coroutines and Asio's coroutine glue. No platform-specific workarounds are needed.
- The model bleeds into types: `awaitable<Result<T>>` is everywhere. New contributors must learn it before writing a feature. We accept this as the cost of a single, consistent style.

## Alternatives Considered

### Option A: Callbacks (Asio classic completion handlers)

The Asio idiom before C++20 coroutines: every async function takes a completion handler `void(error_code, T)`.

- **Pros:** Maximum portability (works on any C++17 compiler); zero compile-time cost beyond the rest of Asio; mature.
- **Cons:** A chain of N callbacks produces an N-deep nest of lambdas, each capturing state by hand and forwarding the completion handler. Cancellation has to be threaded manually through every layer. Lifetime management of captured state is the source of approximately every other CVE in real-world projects that use this pattern. Reviewing a five-step pipeline is hostile.
- **Rejected because:** the readability and lifetime-safety costs swamp the portability gain. We require C++23 already.

### Option B: `std::future` and `.then`-style continuations

`std::async`, `std::future`, `std::shared_future`, with a hypothetical `.then` (which is not in the standard yet — the `std::executions::then` from P2300 is).

- **Pros:** Familiar to almost every C++ developer; small surface area.
- **Cons:** `std::future` has poor cancellation (`std::stop_token` only via `std::jthread`, not via future itself). `std::async` is a blocking-thread runtime that ignores the io_context. The standard library lacks composition operators (`when_all`, `when_any`) for non-`std::execution` futures. The only sane way to compose futures is to spawn a thread per future, which defeats the purpose of an async runtime.
- **Rejected because:** the standard async story isn't yet a coherent system. Building on it would mean reinventing what Asio already gives us.

### Option C: `std::execution` (senders / receivers, P2300)

The new C++26 standard async model. Senders represent async computations; receivers consume their results.

- **Pros:** Heading for standardization; structured concurrency primitives (`when_all`, `let_value`, `stop_token` propagation) are designed-in; competing implementations (libunifex, stdexec) are usable today.
- **Cons:** Not yet in `<execution>` on all four target toolchains. Mature implementations require recent versions and may not match the eventual standard exactly. The sender/receiver model has a steeper learning curve than coroutines — composition is via algorithm chaining rather than imperative `co_await`. The benefit (algorithmic guarantees, customisation points) is more valuable for a library than for an application. Most of our use cases are linear chains, which coroutines handle elegantly already.
- **Rejected because:** the migration cost is too high *today* for a benefit that is mostly theoretical for our workload. We revisit this when (a) C++26 ships and (b) `std::execution` is available on all four toolchains with consistent behaviour. The Asio coroutine code can be ported to senders without architectural change because both centre on the same composition unit: an async operation that yields a `Result`.

### Option D: A custom job queue + thread pool

The pattern used by the original Python mirror bots and by many "C++ Telegram bot" GitHub projects. A `std::queue<std::function<void()>>` guarded by a mutex, a fixed pool of `std::thread`s draining it.

- **Pros:** Familiar; no extra dependencies; conceptually simple.
- **Cons:** Reinvents Asio's `io_context::post` poorly. Cancellation must be added by hand. Backpressure must be added by hand. Composition (`do this, then that, then that other thing`) must be added by hand. The result is a less correct version of what Asio already provides.
- **Rejected because:** it is the wrong abstraction for the workload, and writing it well is harder than learning Asio coroutines.

---

The decision can be revisited if `std::execution` ships and reaches feature parity on all four target toolchains. Coroutine code is largely portable to senders because the unit of composition (`awaitable<Result<T>>` ↔ `sender<Result<T>>`) is structurally similar.
