# ADR-0002: Persistence — SQLite in WAL mode with sqlite-modern-cpp

- **Status:** Accepted
- **Date:** 2026-05-18
- **Deciders:** Engineering team

## Context

CMLB needs persistent state for:

- The `Task` aggregate (id, owner, source, destination, state, progress, timestamps, last error).
- Per-user settings (`UserSettings`: default destination, rename pattern, log verbosity).
- Bot-wide settings (`BotSettings`: runtime-editable subset of config).
- RSS subscriptions and their dedup table of already-delivered entry GUIDs.
- Schema metadata (the current migration version).

The workload characteristics:

- **Tiny dataset.** Even a busy operator deployment is well under 100 MB total. The full set of tasks across a year of activity is on the order of 10⁵ rows, most of them in the `Completed` or `Cancelled` terminal state.
- **Single writer process.** CMLB runs as one process per database. There is no horizontal scale-out scenario in v1 and no design pressure for it.
- **Many small reads.** `/status` queries a few rows. `/stats` aggregates over a small recent window. Repositories are called dozens of times per second under load.
- **Few writes per second.** Each task transition produces a handful of writes. Even at full saturation we're below 100 writes/sec.
- **Operator-friendly.** A non-developer operator should be able to back up, restore, and inspect the database without provisioning a separate service. `sqlite3` is on every Unix.

The candidates:

- SQLite (file-based, embedded, single binary).
- PostgreSQL (server-based, networked, full SQL).
- MongoDB or other document store.
- A custom append-only log of JSON events.

For the access library, the candidates were sqlite-modern-cpp, sqlite_orm, sqlpp11, the raw SQLite C API, and SOCI.

## Decision

CMLB uses **SQLite in WAL mode** as the sole persistent store, accessed via **sqlite-modern-cpp**. The schema is managed by forward-only versioned migrations in `migrations/V<NNNN>__<slug>.sql`. The application keeps a small pool of read connections plus one dedicated writer connection. All connections set the following pragmas at open:

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
PRAGMA temp_store = MEMORY;
PRAGMA mmap_size = 268435456;
```

Schema evolution is forward-only. To fix a mistake in `V0003`, write a `V0004` that corrects it. There is no `down` script and no rollback machinery. The current schema version is stored in a `schema_version` table and checked at startup. The bot refuses to start against a newer schema than it understands; it offers to migrate forward against an older schema (controlled by `database.migrate_on_start`).

Repositories are one-per-aggregate (`TaskRepository`, `UserSettingsRepository`, `BotSettingsRepository`, `RssFeedRepository`). They consume and return domain types; `sqlite_modern_cpp` symbols never leak past the repository's `.cpp` file.

## Consequences

### Positive

- **Zero operational overhead.** No separate service to install, configure, monitor, or back up. The database is a file. Stop the bot, copy the file, start the bot.
- **WAL gives multi-reader concurrency.** Readers don't block the writer; the writer doesn't block readers. For our workload (many small reads, infrequent writes) this is the perfect fit.
- **Crash-safe.** WAL plus `synchronous = NORMAL` survives application crashes; it can lose at most the last few transactions on OS-level crash. For a download bot that is more than acceptable. `synchronous = FULL` would be paranoid for our durability requirements.
- **`PRAGMA foreign_keys = ON`.** Referential integrity is enforced (`tasks.owner_id` references `users.id`, etc). This catches a class of bugs where an aggregate is deleted but its references aren't.
- **Operator inspection is trivial.** `sqlite3 cmlb.db 'SELECT * FROM tasks WHERE state="Failed";'` works. No client setup, no credentials.
- **`sqlite-modern-cpp` is a thin idiomatic wrapper.** Prepared statement binding via `<<` and result reading via `>>` is concise and type-checked. No ORM-style class metaprogramming.
- **Forward-only migrations are honest.** Down migrations are a fiction in practice: you almost never run them in production, they are hard to keep in sync with forward migrations, and they reward authors who write reversible-but-actually-broken pairs. Forcing forward-only forces the team to actually think about migration correctness up front.
- **Live backup is supported.** `sqlite3 cmlb.db .backup ...` is consistent without quiescing.

### Negative

- **Single-host.** SQLite cannot scale to multiple bot processes against the same database. If we ever need that (we do not), this decision must be revisited.
- **Schema migrations are append-only.** Renaming a column requires a multi-step ALTER (recent SQLite versions improved this, but it is still more involved than in PostgreSQL).
- **No native JSON type before SQLite 3.45.** We use SQLite 3.45+ (pinned via vcpkg) so this is moot, but anyone using an old system SQLite would hit it.
- **No row-level locking.** The single writer is global. For our write rate this is irrelevant.
- **Migration tooling is in-house.** We don't get Flyway or Liquibase for free; we wrote a small `SchemaMigrator` ourselves. It is ~200 lines of code and well-tested, but it is one more thing to maintain.

### Neutral

- The single-binary deployment story aligns with how CMLB is shipped (one process, one config file, one database file). SQLite reinforces that.
- The choice locks us to a relational schema. We could not flip to event sourcing without changing the persistence layer. We do not want to flip; this is a non-issue.
- WAL mode produces `-wal` and `-shm` sidecar files. `cp` of the main file alone is not safe during operation; this is documented in [`runbook.md`](../runbook.md#backups) along with the correct `.backup` procedure.

## Alternatives Considered

### Option A: PostgreSQL

- **Pros:** Full-featured SQL, mature, well-known, JSONB type, partial indexes, full-text search out of the box.
- **Cons:** Requires running and operating a separate service. The operator must install it, configure listeners, manage authentication, run backups, monitor disk space — for a workload that is well under 100 MB of data and below 100 writes/sec. The operational surface is many times the application's surface.
- **Rejected because:** the cost-benefit doesn't make sense at our scale. We can revisit if we ever need multi-host or if the dataset grows by three orders of magnitude.

### Option B: MongoDB (or another document store)

- **Pros:** Schema-flexible; might match the `Task` aggregate's heterogeneous shape.
- **Cons:** Same operational overhead as PostgreSQL. Worse: no transactions across documents in single-replica mode without replica-set machinery, weaker durability defaults, a query language that is foreign to most operators. CMLB's data is genuinely relational (tasks reference users, RSS feeds reference users), and the supposedly-flexible schema becomes ad-hoc validation code in our application.
- **Rejected because:** the data is relational and our scale doesn't justify a database server.

### Option C: Custom append-only event log (JSONL on disk)

- **Pros:** No dependency; trivial to inspect with `tail`; matches the "the task aggregate is a state machine" mental model exactly.
- **Cons:** Reinventing storage. We'd write our own indexing for "find active tasks owned by user X", our own compaction story, our own crash recovery. Querying for `/stats` becomes a full scan or a custom index. We'd have written a worse SQLite by hand.
- **Rejected because:** SQLite already does everything this would do, better, and is one library link away.

### Option D: Raw SQLite C API

- **Pros:** Zero dependency beyond SQLite itself; full control; no abstraction surprises.
- **Cons:** Each prepared statement is ~5 lines of boilerplate. `sqlite3_step` / `sqlite3_column_*` / `sqlite3_finalize` everywhere. Easy to forget `sqlite3_finalize`, leaking statements. Forcing every developer to write this is a productivity drag and a bug source.
- **Rejected because:** the cost of a thin wrapper (sqlite-modern-cpp, ~1500 lines of header-only code) is negligible compared to the boilerplate saved.

### Option E: sqlite_orm

- **Pros:** Type-safe queries built from C++ syntax; refactor-friendly; no SQL strings to keep in sync.
- **Cons:** Heavy template metaprogramming → multi-second compile time hit per translation unit that touches the schema; awkward to express raw SQL when needed; the ORM's mental model occasionally collides with SQLite's idioms. CMLB's schema is small, stable, and benefits more from `SELECT` strings being grep-able than from compile-time type checks on column lists.
- **Rejected because:** the costs (compile time, ORM-leakage) outweigh the benefits at our scale.

### Option F: sqlpp11

- **Pros:** Strongly-typed SQL DSL; embedded in C++.
- **Cons:** Same compile-time cost as sqlite_orm; the DSL is verbose; raw-SQL escape hatch is not first-class. Adds a code-generator step for tables.
- **Rejected because:** the workflow added more friction than it removed.

### Option G: SOCI

- **Pros:** Multi-backend (SQLite, PostgreSQL, MySQL) abstraction; nice C++ syntax.
- **Cons:** We don't need multi-backend support; the abstraction has a cost in both compile time and runtime; the C++ API is dated by current standards.
- **Rejected because:** an abstraction we will never exercise is dead weight.

---

The decision can be revisited if the database file approaches 1 GB or if we genuinely need multi-process write access. For the foreseeable future, neither is plausible.
