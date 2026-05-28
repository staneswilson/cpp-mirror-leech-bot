# ADR-0006: Interfaces are `XxxInterface`, not `IXxx`

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

CMLB has a number of abstract base classes that exist purely to be implemented by infrastructure adapters: the type the application layer talks to a downloader through, the type for uploaders, the repositories, the messenger, the system metrics provider, and so on. There is no language-level concept of "interface" in C++ — these are simply classes with `virtual` pure functions — but the role they play in the architecture is the role that `interface` plays in Java, C#, or Go.

We need a naming convention that:

1. **Signals "this is a port"** to readers, so that the application layer's dependence on `DownloaderInterface` rather than `Aria2Downloader` is obvious at the use site.
2. **Encodes the relationship between header file and class name** so that locating either is mechanical.
3. **Survives review and code grep** without producing false positives (e.g. `If`, `Identifier`, `Image` accidentally matching).
4. **Reads naturally** in English when written into code, log messages, and prose.

The candidate conventions:

- `IXxx` (Hungarian-style prefix). Used by C# / Java's COM and parts of legacy MSFT C++.
- `XxxInterface` (suffix). Used by some Google C++ style guides, especially around mocking.
- `XxxImpl` for implementations, with the abstract type having no suffix at all (`Downloader` is the interface; `Aria2Downloader` is the implementation).
- A namespace marker (`ports::Downloader`, `adapters::Aria2Downloader`).
- A language-level keyword (which C++ does not have).

## Decision

CMLB names abstract base classes with the suffix **`Interface`**. The file lives at the equivalent snake_case path. Concrete implementations have descriptive names ending in nouns derived from the implementation, **without** an `Impl` suffix.

Examples:

| Abstract type | File | Concrete implementations |
|---|---|---|
| `DownloaderInterface` | `include/application/downloader_interface.hpp` | `Aria2Downloader`, `QbittorrentDownloader` |
| `UploaderInterface` | `include/application/uploader_interface.hpp` | `TelegramUploader`, `GoogleDriveUploader`, `RcloneUploader` |
| `TaskRepository` (already a clear role-noun) | `include/application/task_repository.hpp` | `SqliteTaskRepository` |
| `Messenger` (already a clear role-noun) | `include/application/messenger.hpp` | `TelegramMessenger` |

A pragmatic exception: when a class name **already reads as a role** (`Repository`, `Messenger`, `Parser`, `Renderer`, `Dispatcher`), the `Interface` suffix is omitted. The role-name is itself the signal. This handles the common case where the role is genuinely the abstract noun.

The `I`-prefix is forbidden everywhere.

## Consequences

### Positive

- **The suffix is unambiguous at the use site.** `void execute(DownloaderInterface& dl)` is plainly a port-dependency. `Aria2Downloader` is plainly a concrete implementation.
- **Grep works.** `grep -r "Interface" include/` lists every port. `grep -r "IDownloader"` is a noisy mess on most codebases because `I` is a 1-character prefix that matches `If`, `Id`, `Identifier`, `Image`, `Index`, ...
- **Reads naturally in prose.** "`MirrorUrl` depends on `DownloaderInterface`" is grammatical English. "MirrorUrl depends on IDownloader" requires the reader to mentally expand the prefix every time.
- **No `Impl` suffix on the concrete class.** The concrete class's name describes what it *is* (`Aria2Downloader`), not its relationship to its abstract base (`DownloaderImpl`). The relationship is captured by the class hierarchy and by the file location (`infrastructure/aria2_downloader.cpp` implements `application/downloader_interface.hpp`).
- **Header-file mapping is mechanical.** `DownloaderInterface` lives at `<layer>/downloader_interface.hpp`. There is no convention to remember beyond `snake_case` per file.

### Negative

- **`Interface` is six characters of overhead per use.** Long type names get longer. Mitigated by the fact that we don't sprinkle these types into hot loops; they show up in function signatures, dependency-injection wiring, and tests.
- **The exception for role-nouns is a judgement call.** Is `RssDocumentParser` a role-noun or should it be `RssDocumentParserInterface`? We decided role-noun, but the line between "obviously a role" and "needs the suffix" requires brief discussion on borderline cases. The rule of thumb: if removing the suffix produces a word that is *already* an abstract role in everyday English (`Parser`, `Renderer`, `Repository`, `Dispatcher`, `Messenger`), drop the suffix.
- **It's longer than `IXxx`.** This is true and irrelevant.

### Neutral

- The convention is one of two well-known approaches (the other being `IXxx`); engineers familiar with either will adapt to the other in minutes.
- The file extension and casing (`.hpp` for C++ headers, `snake_case`) is independent of this decision and covered by the general style guide.

## Alternatives Considered

### Option A: Hungarian `IXxx` prefix

The Java / C# convention.

- **Pros:** Short; familiar to many engineers; appears in established C++ codebases too.
- **Cons:** Hungarian notation in C++ is generally considered a code smell — type prefixes on identifiers fight against the language's strong static typing. A `IDownloader*` is not more informative than a `DownloaderInterface*`. The 1-character prefix is also genuinely awkward to grep for because so many ordinary identifiers start with `I`. The strongest argument against `IXxx` is that it is a relic of an era when languages didn't have a real `interface` keyword and people compensated with a typographical hack; C++ doesn't have a real `interface` keyword either, but we don't need to inherit the workaround.
- **Rejected because:** the costs (grep noise, awkward prose, Hungarian-notation smell) outweigh the brevity benefit.

### Option B: No suffix on the abstract type, `Impl` suffix on the concrete

The abstract type is `Downloader`; the concrete is `Aria2DownloaderImpl` or `DownloaderImpl`.

- **Pros:** Clean abstract name; reads well in dependency declarations (`void execute(Downloader& dl)`).
- **Cons:** The concrete type carries a suffix that adds no information. `Aria2DownloaderImpl` is no clearer than `Aria2Downloader`. Worse, when there is only one implementation (`DownloaderImpl`), the suffix is the *only* differentiator — and it doesn't say anything about what the implementation actually does. The chosen scheme reverses the burden: concrete types carry descriptive names; abstract types signal their role with `Interface`.
- **Rejected because:** the cost of a vague suffix on every concrete class is higher than the cost of a descriptive suffix on every abstract class. There are usually more concrete classes than abstract ones.

### Option C: Namespace-based marker (`ports::Downloader`, `adapters::Aria2Downloader`)

Distinguish via namespace rather than type name.

- **Pros:** No suffix anywhere; namespace already exists for other reasons.
- **Cons:** At the use site, the namespace is often stripped (via `using` or just because it's already in scope). The marker is invisible to the reader. The signal we want is *at the use site*, not at the declaration. Namespace markers don't deliver it.
- **Rejected because:** the signal we wanted to add is invisible to the reader who matters: the one looking at `void execute(Downloader& dl)`.

### Option D: A language-level marker

If C++ had `interface`, we'd use it. C++ does not have `interface`. C++23 has `concept`, which serves a related purpose (type erasure / duck-typing-at-compile-time), but concepts and interfaces solve different problems: concepts are templates, interfaces are runtime polymorphism. We need runtime polymorphism here (mock implementations in tests, configuration-driven adapter selection at startup).

- **Pros:** Compiler-checked role.
- **Cons:** Doesn't exist in C++.
- **Rejected because:** the language doesn't offer it. We do the next-best thing: a consistent naming convention enforced by code review.

---

The `Interface` suffix convention is enforced by code review, not by tooling. We considered a clang-tidy check for `class IXxx` but didn't write one — the convention is easy to spot in PRs, and inventing the check is more work than the violations would cost.
