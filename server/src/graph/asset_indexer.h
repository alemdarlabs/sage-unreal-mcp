#pragma once

#include "graph/graph_store.h"

namespace sage::graph {

// T1 entity ingest. The plugin scans AssetRegistry and ships an array of
// `{path: string, kind: string}` records; this function is the canonical
// path for writing them into a slot's graph.
//
// Strategy: full wipe + batched insert.
//   1. `MATCH (a:Asset) DETACH DELETE a;` clears the previous snapshot
//      (Phase 2.2 has no relationships yet, but DETACH is forward-safe).
//   2. Assets are inserted in batches of 200 via a single multi-pattern
//      `CREATE` statement to amortise the per-query overhead.
//   3. The `_IndexState` row is upserted with `last_indexed_at_ms` (server
//      wall clock) and the new `asset_count`.
//
// Returns Json{{"asset_count", N}, {"last_indexed_at_ms", T}} on success;
// any kuzu error short-circuits and surfaces unchanged.
//
// Idempotent: a re-run with the same input produces the same final state.
[[nodiscard]] GraphResult ingestAssets(GraphStore& store, const Json& assets);

// Read the `_IndexState` row. If the slot has never been indexed, returns
// `{"asset_count": 0, "last_indexed_at_ms": null}`.
[[nodiscard]] GraphResult getIndexStatus(GraphStore& store);

}  // namespace sage::graph
