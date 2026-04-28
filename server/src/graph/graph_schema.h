#pragma once

#include "graph/graph_store.h"

#include <string>
#include <vector>

namespace sage::graph {

// Forward-only schema migration. Each entry's `cypher` runs in order; the
// `_SchemaVersion` node tracks the highest applied version. Migrations are
// idempotent at the DDL level (use IF NOT EXISTS) so a partial apply that
// crashed mid-way can replay safely.
struct Migration {
    int         version;
    std::string cypher;  // semicolon-separated DDL/DML
};

// Highest version known to this binary. Bumped whenever a new migration is
// appended. Stores at this version are up-to-date.
inline constexpr int kCurrentSchemaVersion = 4;

// Registry of every migration the binary knows. Sorted by version ascending.
[[nodiscard]] const std::vector<Migration>& schemaMigrations();

// Bring a freshly-opened (or already-open) store up to kCurrentSchemaVersion.
// On success returns Json{{"from", X}, {"to", Y}, {"applied", N}}; on failure
// returns the first migration error (state may be partial — replay-safe).
[[nodiscard]] GraphResult migrateToCurrent(GraphStore& store);

}  // namespace sage::graph
