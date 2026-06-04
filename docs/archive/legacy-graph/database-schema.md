# Database Schema

> Status: Active storage after ADR-018. KuzuDB has been removed from the build, runtime, tests, and plugin.

## Active Storage

Sage currently keeps only lightweight persistent server state:

- SQLite-backed audit / slot metadata where implemented.
- Filesystem layout under `SAGE_DATA_DIR` / `~/.sage-mcp` for local runtime data.
- Build and runtime logs in the configured output locations.

There is no active `graph.kuzu` directory, Kuzu schema migration, Cypher validator, or graph store abstraction in the runtime.

## Removed Storage

ADR-018 retired the previous KuzuDB layer:

- `third_party/kuzu`
- `kuzu::kuzu` CMake imported target
- `sage-graph` target
- `server/src/graph/*`
- Kuzu smoke tests and graph manager tests
- `scripts/fetch-kuzu.*`
- `graph.kuzu` slot database expectation

## Current Query Strategy

Project understanding now comes from source-backed and editor-backed tools:

- Unreal AssetRegistry-backed asset listing/search/properties.
- Reflection and CDO inspection tools.
- Project and engine C++ source readers/searchers.
- Domain tools for Blueprint, material, animation, Niagara, UMG, level, gameplay, GAS, and other surfaces.
- Logs, crash forensics, compile diagnostics, and runtime state tools.

Any future persistent index must be introduced by a new ADR and must define storage, migration, build, distribution, and verification boundaries from scratch.

## See Also

- [ADR-018: Remove KuzuDB Graph Layer](../../adr/adr-018-remove-kuzudb-graph-layer.md)
- [Architecture](../../architecture/architecture.md)
- [API Spec](../../engineering/api-spec.md)
