#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <cmlb/core/error.hpp>
#include <cmlb/infrastructure/persistence/sqlite_connection_pool.hpp>

/// @file schema_migrator.hpp
/// @brief Applies the embedded SQL migration registry to a SQLite database.
///
/// The migrator owns a compiled-in, ordered registry. Each entry has a
/// monotonically increasing integer version, a stable name (used purely for
/// logging / human inspection), and the SQL body. Applied versions are
/// recorded in the `schema_version` table; subsequent runs skip entries whose
/// version is already recorded, making `migrate()` idempotent.

namespace cmlb::infrastructure::persistence {

/// A single migration entry in the registry.
struct Migration {
    int               version;  ///< Strictly increasing; gaps are not permitted.
    std::string_view  name;     ///< e.g. "V0001__initial_schema".
    std::string_view  sql;      ///< Raw SQL, may contain multiple statements.
};

class SchemaMigrator {
public:
    explicit SchemaMigrator(SqliteConnectionPool& pool) noexcept : pool_{pool} {}

    /// Applies every registered migration whose version is greater than the
    /// max version currently recorded in `schema_version`. Each migration runs
    /// inside its own transaction; a failure aborts the migration in progress
    /// and returns immediately without touching later migrations.
    [[nodiscard]] boost::asio::awaitable<core::Result<void>> migrate();

    /// Reads the highest version recorded in `schema_version`. Returns 0 if
    /// the table is empty or does not yet exist.
    [[nodiscard]] boost::asio::awaitable<core::Result<int>> current_version();

    /// Inspect the compiled-in registry (mostly for tests). Returns a
    /// reference whose lifetime is tied to the `SchemaMigrator`.
    [[nodiscard]] static const std::vector<Migration>& registry() noexcept;

private:
    SqliteConnectionPool& pool_;
};

}  // namespace cmlb::infrastructure::persistence
