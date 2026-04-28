#pragma once

#include "graph/graph_store.h"

namespace sage::graph {

// T1+T2+T3 snapshot ingest. The plugin scans AssetRegistry and the UClass
// reflection registry and ships
//   { assets:        [{path, kind}, ...],
//     dependencies?: [{from, to}, ...],
//     classes?:      [{name, parent, module, is_native}, ...] }
//
// Strategy: full wipe + batched insert across three layers (Asset/T1,
// DEPENDS_ON/T2, Class+INHERITS_FROM/T3).
//   1. Wipe DEPENDS_ON + INHERITS_FROM edges.
//   2. `MATCH (a:Asset) DETACH DELETE a` and `MATCH (c:Class) DETACH DELETE c`
//      clear the previous snapshots.
//   3. Assets, classes, and edges insert in 200-batches with multi-
//      pattern CREATE / UNWIND-MATCH-CREATE statements.
//   4. Dependencies and class parents that reference unknown endpoints
//      are silently skipped (engine internals).
//   5. `_IndexState` upserted with `asset_count`, `dep_count`,
//      `last_indexed_at_ms`.
//
// Returns Json{{"asset_count", N}, {"dep_count", D},
//              {"class_count", C}, {"class_edge_count", E},
//              {"last_indexed_at_ms", T}}.
//
// Idempotent: a re-run with the same input produces the same final state.
[[nodiscard]] GraphResult ingestSnapshot(GraphStore& store, const Json& snapshot);

// Read the `_IndexState` row. If the slot has never been indexed, returns
// `{"asset_count": 0, "dep_count": 0, "last_indexed_at_ms": null}`.
[[nodiscard]] GraphResult getIndexStatus(GraphStore& store);

}  // namespace sage::graph
