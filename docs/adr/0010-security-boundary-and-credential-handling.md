# ADR-0010: Security Boundary — Credentials, TDLib Isolation, Logging Redaction

- **Status:** Accepted
- **Date:** 2026-05-21
- **Deciders:** Engineering team
- **Supersedes:** none
- **Related:** [ADR-0003 Telegram gateway isolation](0003-telegram-gateway-isolation.md), [ADR-0009 Five-layer DDD](0009-five-layer-ddd-layering.md), [SECURITY.md](../../SECURITY.md)

## Context

CMLB holds three classes of secret at runtime:

1. **Telegram credentials** — `api_id`, `api_hash`, `bot_token`,
   `owner_id`. The bot token grants full control of the bot identity;
   leakage means a stranger can read every chat the bot has joined and
   impersonate it to authorised users.
2. **Cloud credentials** — Google Drive service-account JSON, rclone
   `rclone.conf`, qBittorrent Web UI password, aria2 RPC secret. Each
   unlocks a distinct external resource pool (storage, torrent client,
   download daemon).
3. **TDLib database keys** — the encryption key TDLib derives at first
   start, stored under `tdlib/`. Loss means re-authentication; theft
   means an attacker can replay the session offline.

Each of these has historically leaked from comparable Python bots:
hard-coded into `.py` files, echoed onto stdout by `print(config)`,
captured by uncaught-exception traces, or accidentally committed to
public Git history. The mitigations have to be structural — depending
on every contributor remembering to scrub a log line is not a plan.

A second boundary exists at the TDLib edge. TDLib is a sprawling library
with its own threading model, its own `td_api::object_ptr` ownership
discipline, and its own header-only generated API surface
(`<td/telegram/td_api.h>` brings in tens of thousands of lines of
templates). Including it widely makes the build slow, the diagnostics
noisy, and the cancellation story ambiguous (TDLib uses its own
`ClientManager::receive` loop independent of our Asio executor).

## Decision

### 1. Credential boundary

- **Sources.** Secrets enter the process exclusively from two sources:
  the `config.json` file pointed to on the command line, and
  `CMLB_*` environment variables that override matching JSON fields.
  Nothing else — not stdin, not a database row, not a hard-coded
  fallback in the source tree.
- **Storage in memory.** Secrets live in the `cmlb::core::AppConfig`
  struct returned by `Configuration::load`. The struct is read-only
  after load. There is no global secret cache; the secrets travel by
  reference (or by clone of just the required string) into the
  adapters that need them, and stay there.
- **Logging.** `cmlb::core::Logger` (spdlog) never receives a secret
  directly. The configuration loader masks secrets when echoing the
  effective config at startup (`"bot_token": "***"`). Any new
  log call that interpolates a config field has to consult the
  redaction utility; PRs that bypass it are rejected.
- **Exception traces.** `AppError::message` is human-readable English
  and never includes a credential. The convention "say the cause,
  not the operation" (see ADR-0008) makes this easy to police: a
  message saying `"Bad bot token"` is wrong; `"Telegram rejected
  authorisation"` is right.
- **Source control.** `config.json`, `.env`, `service_account*.json`,
  `tdlib/`, and `data/cmlb.db*` are in `.gitignore`. `pre-commit`
  hooks scan staged content for the literal patterns
  `bot_token`, `api_hash`, `BEGIN PRIVATE KEY`. Commits that match
  fail before pushing. The same scan runs in CI as a belt-and-braces
  check.
- **On-disk permissions.** `config.json` must be `0600` and owned by
  the service user; the systemd unit and Dockerfile both create the
  service user with `useradd -r -s /usr/sbin/nologin`. The bot
  refuses to start if `config.json` is world-readable.

### 2. TDLib isolation

- **One translation unit.** `<td/telegram/td_api.h>` is included from
  *exactly one* `.cpp` file:
  `src/infrastructure/telegram/telegram_gateway.cpp`. Every other
  layer interacts with TDLib through `TelegramGateway`'s public
  signatures, which use only domain and core types.
- **Strand discipline.** All TDLib calls live on a dedicated
  `asio::strand` owned by the gateway. Any code that touches a TDLib
  object `co_await`'s onto that strand first. This makes the
  ownership model defensive against TDLib's own thread-of-execution
  rules: we never call into TDLib from a coroutine resumed on
  another executor.
- **Lifetime.** TDLib is constructed once, in
  `TelegramGateway::start`, and torn down once, in `stop`. There is
  no global static. Two instances would race on the `tdlib/`
  directory.
- **Header probe.** CI runs a `clang-tidy`/IWYU pass that fails if any
  source file outside the gateway TU includes a `td/...` header. This
  prevents the TDLib include from leaking back into the build graph
  via a refactor.

### 3. Logging redaction at sinks

- spdlog is configured with two sinks: a rotating file
  (`logs/cmlb.log`, 10 MiB × 5) and a colored stderr sink. Both are
  the same logger; we don't have an alternate "debug" sink that
  skips redaction.
- The configuration loader applies redaction *before* the value
  reaches the logger: `bot_token`, `api_hash`, `password`,
  `client_secret`, `rpc_secret`, `service_account` body fields are
  replaced with `***` in any structured echo.
- Errors propagated from TDLib (`error.message`) are wrapped through
  `friendly_error_label` for chat copy and logged in full only at
  `debug` level. A leaked token in a TDLib error string still ends
  up in `logs/cmlb.log` at debug level — which is why log files are
  owned by the service user with `0600` permissions and aggregated
  through the redaction filter described in `docs/runbook.md`.

### 4. Boundary review on PR

Every PR touching the credential or TDLib boundary requires a
checklist review:

- [ ] No new `#include <td/...>` outside the gateway TU.
- [ ] No new `Logger::*` line interpolating a config field without
      redaction.
- [ ] No new file path resembling `*token*`, `*secret*`,
      `*credential*` outside `.gitignore`'d paths.
- [ ] If a new external secret is added, it has a `CMLB_*` env-var
      override and a documented redaction rule.

## Consequences

### Positive

- The blast radius of a future TDLib breaking change is contained to
  one file. We have one upgrade path, not twelve.
- Build times improve substantially: TDLib's templates are heavy and
  excluding them from twelve TUs saves measurable wall clock per
  rebuild.
- Operators can hand `cmlb.log` to a third party (cloud log
  aggregator, support ticket) with confidence that credentials are
  not present at default log levels.
- The credential surface is auditable from one place
  (`include/cmlb/core/configuration.hpp` + the loader). New secrets
  *can't* enter the process via an undocumented side channel.

### Negative

- TDLib types can't appear in public infrastructure interfaces.
  We translate everything to domain values (`MessageId`, `ChatId`,
  `UserId`) at the gateway boundary, which costs a small amount of
  marshalling per call. The trade is worth it.
- Redaction at the loader means a log line saying "config loaded"
  carries `***` placeholders. Operators occasionally complain that
  this makes "is my token even set?" harder to debug. The answer is
  a separate `cmlb --validate-config <path>` command that reports
  the *shape* of the loaded config (which fields are present /
  missing) without echoing values.

## Alternatives considered

1. **Vault / KMS-backed secret loading at startup.** Deferred.
   Operators with a Vault deployment can mount secrets as
   environment variables; building a first-party Vault client is out
   of scope for v1.
2. **TDLib via JSON IPC over a Unix socket to a sidecar process.**
   Rejected for v1: doubles the process count, doubles the failure
   modes, doesn't materially improve the security posture (the
   sidecar still holds the token). May revisit if TDLib's footprint
   becomes a tail-latency problem.
3. **Encrypted log file at rest.** Rejected: depends on a master key
   that has to live somewhere, which restates the original problem.
   Operators can use the OS-level filesystem encryption that already
   protects `tdlib/` and `data/cmlb.db`.

## Implementation notes

- `include/cmlb/core/configuration.hpp` lists every secret field.
  The loader in `src/core/configuration.cpp` applies redaction on
  the `to_string`/`fmt::format` paths.
- `src/infrastructure/telegram/telegram_gateway.cpp` is the sole TU
  that includes `<td/telegram/td_api.h>`. All other gateway-side
  code in `src/infrastructure/telegram/` includes the gateway header
  by reference, not the TDLib header.
- `SECURITY.md` documents the reporting channel
  (GitHub private security advisories), the supported versions, and
  the operator-side hardening checklist.
- `.pre-commit-config.yaml` and the CI `secret-scan` workflow run
  the same regex set; divergence between them is treated as a CI bug.
