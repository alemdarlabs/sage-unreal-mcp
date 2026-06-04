# Knowledge Graph

> Status: Retired by ADR-018. The active Sage runtime no longer contains a KuzuDB-backed knowledge graph.

## What Changed

The previous graph subsystem indexed assets, dependencies, class hierarchy data, and reference topology into KuzuDB. That subsystem has been removed completely from active code and build outputs.

Removed public tools:

- `index_slot`
- `index_status`
- `impact_of`
- `references_to`
- `find_unused`
- `class_hierarchy`
- `query_graph`

Removed implementation areas:

- `server/src/graph/*`
- `sage-graph` CMake target
- Kuzu fetch scripts
- Kuzu and graph smoke tests
- plugin-side graph delta hooks
- `_scan_asset_registry`

## Current Project Understanding Model

Sage now favors live, source-backed inspection:

- Asset and package discovery through Unreal AssetRegistry-backed tools.
- Reflection and class inspection through Unreal reflection APIs.
- Blueprint safety through `bp.full_dump` and focused Blueprint authoring tools.
- Source-level research through project/engine C++ readers and search tools.
- Domain-specific diagnostics for animation, Niagara, material, UMG, gameplay, GAS, level, and editor state.

This keeps tool answers tied to the editor/project state the agent is actually mutating and removes KuzuDB distribution/runtime risk.

## Future Work

A future project-understanding layer is allowed, but it needs a new ADR. The default should be source-backed and tool-backed first; persistent indexing must justify its storage engine, migration story, packaging footprint, and failure behavior.

## See Also

- [ADR-018: Remove KuzuDB Graph Layer](../../adr/adr-018-remove-kuzudb-graph-layer.md)
- [Architecture](../../architecture/architecture.md)
- [API Spec](../../engineering/api-spec.md)
