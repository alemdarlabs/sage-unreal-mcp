#pragma once

#include "graph/graph_store.h"

namespace sage::graph {

// T1+T2 snapshot ingest. The plugin scans AssetRegistry and ships
//   { assets: [{path, kind}, ...],
//     dependencies?: [{from, to}, ...] }
//
// Strategy: full wipe + batched insert.
//   1. Wipe DEPENDS_ON edges (explicit; DETACH DELETE on Asset would also
//      drop them but the explicit pass keeps the steps auditable).
//   2. `MATCH (a:Asset) DETACH DELETE a;` clears the previous T1 snapshot.
//   3. Assets in 200-batch multi-pattern `CREATE` statements.
//   4. Dependencies, if present, in 200-batch multi-`MATCH+CREATE` edge
//      statements. Edges referencing paths missing from `assets` are
//      silently skipped (engine assets sometimes reference internals
//      that AssetRegistry doesn't surface).
//   5. `_IndexState` upserted with `asset_count`, `dep_count`,
//      `last_indexed_at_ms`.
//
// Returns Json{{"asset_count", N}, {"dep_count", D},
//              {"last_indexed_at_ms", T}}.
//
// Idempotent: a re-run with the same input produces the same final state.
[[nodiscard]] GraphResult ingestSnapshot(GraphStore& store, const Json& snapshot);

// Read the `_IndexState` row. If the slot has never been indexed, returns
// `{"asset_count": 0, "dep_count": 0, "last_indexed_at_ms": null}`.
[[nodiscard]] GraphResult getIndexStatus(GraphStore& store);

}  // namespace sage::graph
