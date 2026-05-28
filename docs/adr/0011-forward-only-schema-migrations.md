# ADR-0011: Forward-Only Schema Migrations

- **Status:** Accepted
- **Date:** 2026-05-21
- **Deciders:** Engineering team
- **Supersedes:** none
- **Related:** [ADR-0002 Persistence — SQLite WAL](0002-persistence-sqlite-wal.md)

## Context

CMLB's SQLite database is the system of record for tasks, user
settings, bot settings, RSS feeds, and (eventually) audit logs. The
schema will evolve — new fields, new tables, narrower constraints,
indexes added once row counts justify them.

Two migration models are common:

- **Up/down pairs.** Each migration ships a `down` script that
  reverses it. Familiar to anyone who has used Rails / Django.
- **Forward-only.** Each migration has only an `up` script. Rolling
  back means deploying an older binary against the *current* schema,
  which must still be able to read it.

The trade-off is between the *theoretical* ability to roll back the
schema and the *practical* habits the choice encourages. In our
experience:

- Down scripts are written, never tested, and rot. The first time one
  is actually invoked is on a production database after a failed
  deploy — exactly the moment to discover that the script drops a
  column with a foreign-key constraint nobody remembered.
- Reversible migrations encourage destructive forward migrations.
  `DROP COLUMN` becomes "fine, we have a down". In practice rollbacks
  are abandoned in favour of forward-fix migrations, and the down
  script becomes dead weight.
- The actual production rollback pattern is "deploy the previous
  binary, leave the schema." That only works if the previous binary
  can read the *new* schema — i.e. migrations are additive.

CMLB is an embedded-database, single-instance bot. There is no
multi-writer concurrency to coordinate, no Zookeeper-blessed schema
registry. The forward-only model fits the deployment shape.

## Decision

CMLB ships forward-only SQL migrations with the following rules.

### File layout

```
src/infrastructure/persistence/migrations/
    V0001__bootstrap.sql
    V0002__rss_feeds.sql
    V0003__task_status_index.sql
    ...
```

- Filenames match the regex `V[0-9]{4}__[a-z0-9_]+\.sql`.
- Numbers are dense and monotonic — no gaps, no re-numbering.
- The descriptive slug is for humans; the loader keys off the
  numeric prefix only.

### Schema-version tracking

A `schema_version` table holds one row per applied migration:

```sql
CREATE TABLE IF NOT EXISTS schema_version (
    version       INTEGER PRIMARY KEY,
    applied_at    TEXT    NOT NULL,
    description   TEXT    NOT NULL
);
```

At boot, `SchemaMigrator::run()` reads `MAX(version)`, then applies
every `.sql` whose numeric prefix is greater, in ascending order.
Each migration runs in a single transaction (`BEGIN; ...; COMMIT;`)
and inserts its `schema_version` row in the same transaction. A
partial migration cannot leave the DB half-upgraded.

### What migrations are allowed to do

- `CREATE TABLE`, `CREATE INDEX`, `CREATE VIEW`, `CREATE TRIGGER`.
- `ALTER TABLE ... ADD COLUMN` with a default that backfills cleanly.
- Data backfills via `INSERT ... SELECT ...` or `UPDATE` against
  newly-added columns.
- Rewriting a table via the "create new, copy data, drop old, rename"
  dance — but only when an ALTER cannot express the change (SQLite's
  `ALTER` is limited). The rewrite still runs in one transaction.

### What migrations are *not* allowed to do

- `DROP TABLE`, `DROP COLUMN` of a column the previous binary version
  still reads. (If a column is genuinely unused, deprecate it for one
  release, then drop it in the next. The two-step keeps rollback
  viable.)
- Tighten a constraint (`NOT NULL` on a previously-nullable column)
  without first backfilling. The migration body itself does the
  backfill in the same transaction as the constraint change.
- Reference application-layer code. Migrations are pure SQL. No
  embedded `EXEC` of a host-language hook.

### Versioning of the binary

The binary versions itself independently of the schema. A binary at
version `1.2.0` knows the highest schema version it requires
(compiled into `kMinSchemaVersion`). At startup it refuses to run if
the DB schema is *older* than `kMinSchemaVersion`. It happily runs
against a *newer* schema, ignoring the new columns/tables it doesn't
understand — which is what makes binary rollback safe.

### Testing

Every migration ships with at least one test under
`tests/integration/schema_migrator_test.cpp`:

1. The migration applies cleanly to a fresh database (no prior
   schema).
2. The migration applies cleanly on top of schema `N-1` (the previous
   version).
3. After application, the relevant repository's CRUD test still
   passes against the new schema.

`ctest --preset debug -R schema_` runs all three checks.

### CI gate

A CI job spins up a fixture database at each released schema version
and applies every subsequent migration in sequence. A migration that
errors against a real on-disk SQLite file (different from in-memory
in pragmas and constraint timing) fails the build before merge.

## Consequences

### Positive

- Operators upgrade by replacing the binary; the bot applies pending
  migrations on next start and continues.
- Rollback is a binary swap — no schema gymnastics. The newer schema
  remains; the older binary ignores the new bits.
- Migrations stay small. The discipline of "no destructive changes"
  enforces additive design, which makes feature flags / dark
  launches natural.
- The CI matrix exercises real on-disk SQLite behavior (WAL,
  fsync, the locking model) on every PR.

### Negative

- A genuinely wrong schema decision can't be undone in one PR. It
  takes two: one to deprecate, one (a release later) to remove. We
  view this as a feature — schema PRs get more review time as a
  result.
- "Unused" columns accumulate during the deprecation window. Storage
  cost is negligible (SQLite, single host); cognitive cost is real
  but bounded by the column being commented as `-- deprecated in
  Vxxxx, remove in Vyyyy` so reviewers can spot when it's safe to
  drop.
- The "rewrite via temp table" pattern is wordy. We accept the
  verbosity rather than reach for an ORM-driven migration DSL that
  hides what's happening.

## Alternatives considered

1. **Up/down migrations.** Rejected for the reasons in *Context*
   above: rarely tested, encourage destructive forward changes, the
   actual production rollback workflow doesn't use them.
2. **Auto-derived schema from C++ structs.** Rejected: the loss of
   precision (column types, default values, index choices) outweighs
   the boilerplate it saves. We want migrations reviewable as SQL.
3. **External tool (Flyway / Liquibase).** Overkill for a single
   embedded-SQLite bot. Adds a JVM dependency to the operator
   experience.

## Implementation notes

- `src/infrastructure/persistence/migrations/*.sql` files are
  embedded into the binary at build time as a `constexpr` table by
  `cmake/embed_sql.cmake` — operators don't ship a directory of
  scripts alongside the binary.
- `src/infrastructure/persistence/schema_migrator.cpp` opens the
  database, runs the embedded list, and writes a startup log line:
  `"schema: V{n} applied (description)"` for each new migration.
- The bot CLI exposes `--migrate-only`, which runs the migrations
  and exits with code 0 on success, non-zero on failure. Used by the
  systemd unit's `ExecStartPre` so a failed migration prevents the
  main service from coming up.
- `docs/runbook.md` documents the backup-before-upgrade procedure
  (`sqlite3 data/cmlb.db ".backup data/cmlb.pre-upgrade.db"`).
