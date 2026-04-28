# Architecture

> Status: Implementation complete — Phase 1+2+3+4 doğrulandı (2026-04-28 · 82 commit · 443 tool).
> Mimari aşağıda tarif edildiği şekilde inşa edilmiş ve UE 5.7.4'te çalışmaktadır.

## Overview

Sage Unreal MCP is an MCP server that turns Unreal Engine into a first-class peer for AI agents. Two layers:

- **Execution layer** — tool calls that mutate engine state (spawn actor, modify property, save asset, compile module).
- **Intelligence layer** — a project knowledge graph the agent queries before acting (impact analysis, dependency tracing, class hierarchy, reference topology).

The intelligence layer is the moat. Most current MCPs in the engine space stop at execution; Sage treats execution and understanding as inseparable.

## System Components

```
┌──────────────────────────────────────────────┐
│  MCP Client (Claude Code, Cursor, ...)       │
└──────────────────┬───────────────────────────┘
                   │ MCP over HTTP+SSE
                   │ (persistent, multi-client)
                   ▼
┌──────────────────────────────────────────────┐
│  Sage Server (long-lived process)            │
│  ├─ MCP protocol layer                       │
│  ├─ Tool router & operation queue (persisted)│
│  ├─ Knowledge graph (KuzuDB, on-disk)        │
│  ├─ Audit log + slot index (SQLite)          │
│  ├─ Editor lifecycle manager                 │
│  └─ WebSocket server (for plugins)           │
└──────────────────┬───────────────────────────┘
                   │ WebSocket (localhost)
                   │ JSON-RPC envelope
                   │ resilient, auto-reconnect
                   ▼
┌──────────────────────────────────────────────┐
│  Unreal Editor (ephemeral, may restart)      │
│  └─ Sage Bridge Plugin                       │
│      ├─ FWebSocketsModule (client)           │
│      ├─ AssetRegistry listener               │
│      ├─ UTransactor wrapper                  │
│      ├─ Reflection traverser                 │
│      └─ Background indexing workers          │
└──────────────────────────────────────────────┘
```

### Process Model Invariants

- Server is the single source of truth for persisted state. Editor process state is volatile, reconstructable.
- Editor can crash, restart, recompile, or be one of several instances; server survives all.
- MCP client maintains one persistent connection to server; server multiplexes to N editor instances.

## Data Flow

### Tool Call (single-op)

```
Claude → Server: { tool: "modify_actor_property", args: {...}, _editor: "host" }
Server: validate, route to slot, persist op-queue entry
Server → Plugin (host): { tx_id, tool, args }
Plugin: open FScopedTransaction, call UObject::Modify(), apply mutation
Plugin → Server: { tx_id, status: "committed", before/after_hash }
Server: update KuzuDB graph, write audit log entry
Server → Claude: { tx_id, success, undo_handle }
```

### Knowledge Graph Query

```
Claude → Server: { tool: "impact_of", args: { target: "BP_Enemy" } }
Server: KuzuDB Cypher query (incoming edges to BP_Enemy node)
Server: tier-aware response shaping (T1/T2 only by default)
Server → Claude: { results: [...], pagination_cursor }
```

### Editor Restart Orchestration

See [`compile-coordination.md`](compile-coordination.md) for the full restart flow.

## Multi-Editor Support

A single Sage server can be connected to multiple Unreal Editor instances simultaneously. See [ADR-004](../decisions/adr-004-multi-editor.md) for routing model and [knowledge-graph.md](knowledge-graph.md) for slot-shared graph behavior.

Key concepts:
- **Slot ID**: identity hash of `(project_id, canonical_path, engine_major)`. See [ADR-003](../decisions/adr-003-identity-model.md).
- **Active editor pointer**: per-Claude-session, switches via `set_active_editor`.
- **`_editor` parameter**: per-tool override.
- **Disambiguation error**: returned when active is null and `_editor` not given in multi-instance state.

## Lifecycle & Resilience

### Editor Connection State Machine

```
EDITOR_OFFLINE → CONNECTING → HANDSHAKING → CONNECTED
                                              │
                          ┌───────────────────┤
                          ▼                   │
                     EDITOR_LOST              │ (intentional)
                     (grace 30s)              ▼
                          │             RESTARTING
                          ▼                   │
                     EDITOR_DEAD              │
                          │                   │
                          └─── relaunch ──────┘
```

- **EDITOR_LOST** vs **RESTARTING**: a sudden disconnect enters EDITOR_LOST with a 30-second grace; an intentional restart (tool-initiated) enters RESTARTING immediately.
- Heartbeat: plugin → server every 15s; missing 2 consecutive marks the connection lost.

### State Preservation Across Editor Restart

| State | Storage | Survives editor restart? |
|---|---|---|
| Knowledge graph | KuzuDB on disk | Yes |
| Audit log | SQLite on disk | Yes |
| Operation queue (in-flight tool calls) | SQLite on disk | Yes (replayed) |
| Conversation context | Client side | Yes (connection unbroken) |
| Open assets / world | Snapshot on disk | Yes (restored) |
| Editor selection | Snapshot (optional) | Default no |
| Editor undo/redo stack | Editor memory | No (UE limit) |
| Plugin internal cache | Memory | No (rebuilt) |

### Edge Cases

- User manually closes editor → `user_initiated_shutdown` flag, server does not auto-relaunch; client prompted.
- Editor crash mid-transaction → UTransactor cleans partial state on restart; server marks transaction `errored`.
- Compile fails after shutdown → server can relaunch the previous binary and report compile error.
- Server crash with editor running → plugin reconnects with backoff; KuzuDB on disk; resume.

## See Also

- [Tech Stack](tech-stack.md) — language, library, build choices
- [API Spec](api-spec.md) — tool catalog, transports, token optimization
- [Database Schema](database-schema.md) — KuzuDB + SQLite layouts
- [Knowledge Graph](knowledge-graph.md) — indexing strategy, schema
- [Transactions](transactions.md) — transaction layer detail
- [Compile Coordination](compile-coordination.md) — Live Coding vs full restart
- [Decisions](../decisions/) — ADR log
