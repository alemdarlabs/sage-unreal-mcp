# ADR-002: Storage

**Date:** 2026-04-27
**Status:** Superseded by ADR-018

## Context

The original design separated two persistence concerns:

1. A graph store for asset and code relationships.
2. An audit store for operations, sessions, and transaction metadata.

At the time, these concerns appeared to have different access patterns.

## Decision

Use KuzuDB for the graph store and SQLite for audit metadata.

The planned layout was slot-based:

```text
~/.sage-mcp/
|-- slots/
|   `-- <slot-id>/
|       |-- graph.kuzu/
|       `-- audit.sqlite
`-- config.json
```

## Rationale

KuzuDB was selected for embedded graph traversal and Cypher-style querying.
SQLite was selected for durable, transactional, easy-to-debug operational
metadata.

## Consequences

Positive:

- Graph traversal had a dedicated storage engine.
- Audit metadata stayed separate from graph data.
- Slot isolation limited data corruption blast radius.

Negative:

- Two persistence systems increased build, packaging, and backup complexity.
- KuzuDB introduced native runtime distribution risk.
- The graph duplicated data Unreal already exposes through live systems.

## Supersession

ADR-018 removed KuzuDB from the active runtime and build graph. Current Sage
project understanding is live-source-backed and editor-backed. Any future
persistent index requires a new ADR.
