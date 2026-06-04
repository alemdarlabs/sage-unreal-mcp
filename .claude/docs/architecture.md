# Architecture

> Status: Active topology after ADR-018. The KuzuDB-backed graph layer and graph query tools are retired.

## Overview

Sage Unreal MCP is an MCP server that turns Unreal Engine into a first-class peer for AI agents.

The active architecture has two responsibilities:

- **Execution**: route MCP tool calls into the right Unreal Editor instance and apply Unreal-native operations safely.
- **Inspection**: answer project questions through live Unreal APIs, reflection, AssetRegistry-backed tools, source search, logs, and domain-specific diagnostics.

There is no embedded KuzuDB graph database in the active runtime. Future persistent indexing requires a fresh ADR and must not reintroduce KuzuDB by default.

## System Components

```text
+-------------------------------------------+
| MCP Client (Codex, Claude Code, Cursor)   |
+----------------------+--------------------+
                       |
                       | MCP over HTTP + SSE
                       v
+-------------------------------------------+
| Sage Server (long-lived process)          |
| - MCP protocol layer                      |
| - Tool registry and schema shaping        |
| - Multi-editor routing                    |
| - Audit log / slot metadata (SQLite)      |
| - Job orchestration                       |
| - WebSocket bridge for plugins            |
+----------------------+--------------------+
                       |
                       | JSON-RPC over WebSocket
                       v
+-------------------------------------------+
| Unreal Editor + SageBridge plugin         |
| - Tool dispatch                           |
| - FScopedTransaction / UTransactor use    |
| - Reflection and AssetRegistry access     |
| - Blueprint, asset, level, material,      |
|   animation, Niagara, UMG, and project    |
|   operations                              |
+-------------------------------------------+
```

## Process Model

- The server survives editor restarts and reconnects.
- The plugin is editor-local and reconnects to the server over WebSocket.
- Multiple editor instances can be connected at the same time.
- Editor-scoped tools accept `_editor` and are routed explicitly when multiple editors are connected.
- Server-only tools do not accept `_editor`.

## Tool Call Flow

```text
Client -> Server: tools/call { name, arguments, _editor? }
Server: validate schema, resolve editor, enqueue/dispatch
Server -> Plugin: JSON-RPC bridge request
Plugin: run Unreal operation, marshal mutations to GameThread when needed
Plugin -> Server: success/error payload
Server -> Client: normalized MCP response
```

For mutations, the plugin uses Unreal-native transactions where available and returns structured errors rather than silently swallowing partial work.

## Inspection Flow

Source-backed inspection replaces the retired graph database:

- Asset discovery: `asset.list`, `asset.search`, `asset.read_properties`, and domain asset tools.
- Class/reflection discovery: `reflect_class`, `list_classes`, `find_implementers`, `class_default_object`, and related reflection tools.
- Source research: project and engine C++ readers/searchers plus Unreal API reference tools.
- Blueprint safety: `bp.full_dump` before major mutation, reparent, delete, or conversion work.
- Runtime/project state: editor, PIE, logs, crash forensics, world, actor, component, material, animation, Niagara, GAS, UMG, and sequencer tools.

The removed graph tools are: `index_slot`, `index_status`, `impact_of`, `references_to`, `find_unused`, `class_hierarchy`, and `query_graph`.

## State Preservation

| State | Storage | Survives editor restart |
|---|---|---|
| Audit log / slot metadata | SQLite / filesystem | Yes |
| In-flight job metadata | Server memory / job layer | Server-dependent |
| Connected editor identity | Server session state | Rebuilt on reconnect |
| Open assets / world state | Unreal project/editor state | Unreal-dependent |
| Editor undo/redo stack | Editor memory | No |
| Plugin caches | Plugin memory | No, rebuilt |

## Edge Cases

- User manually closes editor: server keeps running; tools that require an editor return a connection error.
- Editor crash mid-transaction: plugin disconnects; server reports the failed call and waits for reconnect.
- Compile/restart path: handled by compile coordination and restart orchestration.
- Server restart: connected plugins reconnect with backoff.

## See Also

- [Tech Stack](tech-stack.md)
- [API Spec](api-spec.md)
- [Database Schema](database-schema.md)
- [Knowledge Graph](knowledge-graph.md)
- [Transactions](transactions.md)
- [Compile Coordination](compile-coordination.md)
- [ADR-018](../decisions/adr-018-remove-kuzudb-graph-layer.md)
