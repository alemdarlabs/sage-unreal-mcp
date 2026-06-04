# MVP Roadmap - Completed

> Historical archive. ADR-018 retired the KuzuDB-backed Phase 2 graph layer.
> Active build and runtime scope must follow ADR-018.

## Status

Phase 1, Phase 2, Phase 3, and Phase 4 were completed by 2026-04-28 in the
original roadmap. This file records the historical plan and should not be used
as the active architecture source.

## Phase 1 - Execution Layer

Actual duration: about one week.

Milestones:

- Server scaffolding: CMake, vcpkg, C++23 baseline, JSON-RPC envelope, MCP tool
  registration, HTTP/SSE transport, logging, tests, and sanitizers.
- Plugin scaffolding: UPlugin layout, WebSocket client, reconnect, heartbeat,
  editor identity, multi-editor labels, and BuildPlugin packaging.
- Core mutation tools: actor, component, asset, editor, level, PIE, material,
  transactions, bulk mutation, optimistic locking, source control, and tests.
- Lifecycle and compile coordination: editor registration, active editor tools,
  Live Coding integration, full restart orchestration, and PIE mutation policy.

Historical exit criteria were satisfied.

## Phase 2 - Retired Knowledge Layer

The original Phase 2 implemented KuzuDB-backed graph indexing, schema migration,
AssetRegistry indexing, dependency edges, real-time delta sync, high-level graph
queries, and a restricted Cypher layer.

ADR-018 supersedes this layer. The current runtime no longer includes KuzuDB or
graph MCP tools.

## Phase 3 - Execution Polish

Phase 3 added restart orchestration and class hierarchy work. The graph-backed
class hierarchy implementation was later superseded by ADR-018.

## Phase 4 - Tool Parity Expansion

Phase 4 was added after UE-MCP parity research. It expanded the tool surface
across:

- Reflection
- Blueprint authoring
- Material graph authoring
- Asset operations
- Editor automation
- Project/source inspection
- Animation
- Niagara
- Gameplay
- PCG
- Landscape
- Foliage
- GAS
- Networking
- Audio
- UMG

Historical state at completion:

- 443 plugin handlers.
- 454 server schemas.
- 445 / 448 UE-MCP parity items, with 3 items treated as not applicable.

These counts are historical. Use `scripts/audit-tools.ps1 -Json` for the
current source truth.

## Retired Phase 5 Candidates

The old roadmap listed:

- End-to-end integration tests.
- Per-domain smoke tests.
- `sage.toml` configuration.
- Public documentation.
- `asset.migrate`.
- MCP transport polish.
- Disconnected `project.get_info`.

Some items were later implemented, changed, or invalidated by follow-up
architecture work. Verify current status from source before acting on this
archive.
