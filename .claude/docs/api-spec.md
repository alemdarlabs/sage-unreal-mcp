# API Specification

MCP tool catalog, transport protocols, and token-optimization principles.

## Transports

### Claude ↔ Server: HTTP + SSE (Streamable HTTP)

- **Endpoint**: `http://<host>:<port>/mcp`
- **Default port**: 7777 (configurable via `SAGE_HTTP_PORT`)
- **Protocol**: MCP over HTTP + SSE per [MCP spec](https://modelcontextprotocol.io)
- **Multi-client**: Yes — server holds multiple concurrent client sessions
- **Streaming**: Tools that generate progressive output (compile, indexing, bulk) stream via SSE chunks

### Server-pushed notifications (Claude ← Server)

Server emits JSON-RPC 2.0 notifications (no `id`) so the agent observes
lifecycle events without polling. Channel:

- **stdio mode**: written to stdout interleaved with responses; `StdioMcp::writeJson`
  serializes both producers behind a mutex so no JSON-RPC line tears.
- **HTTP+SSE mode**: routed over the GET `/mcp` SSE stream (TODO; tracked
  alongside the `Mcp-Session-Id` / `Mcp-Protocol-Version` paketleme blocker).

All sage events use the spec-canonical `notifications/message` envelope so MCP
clients with default logging UIs surface them without custom handlers:

```json
{
  "jsonrpc": "2.0",
  "method":  "notifications/message",
  "params": {
    "level":  "info",
    "logger": "sage.editor",
    "data": {
      "event":          "connected",
      "session_id":     "1",
      "slot_id":        "9780bcd7c32e...",
      "instance_id":    "HeroFlight@8ca84e8c",
      "label":          "",
      "project_path":   "D:/Steamworks/HeroFlight/HeroFlight.uproject",
      "project_id":     "...",
      "engine_version": "5.7.4",
      "pid":            12345
    }
  }
}
```

Current event kinds (`data.event`):

- `connected` — plugin handshake completed; full identity available.
- `disconnected` — bridge socket closed (snapshot taken just before erase).

Future kinds: `indexing_progress`, `editor_log_spike`, `bp_compile_complete`.

Notifications are best-effort: if the MCP harness doesn't surface them as
system reminders, the agent should fall back to `wait_for_editor` (see below)
which is the deterministic blocking-tool path.

### Server ↔ Plugin: WebSocket (localhost)

- **Endpoint**: `ws://localhost:<port>/bridge`
- **Default port**: 7778 (configurable via `SAGE_WS_PORT`)
- **Envelope**: JSON-RPC 2.0
- **Resilience**: Plugin reconnects with exponential backoff (initial 1s, max 30s)
- **Heartbeat**: Plugin → server every 15s; server marks lost after 30s silence

### Handshake (plugin → server)

```json
{
  "type": "hello",
  "version": "0.1.0",
  "editor": {
    "id": "MyProject@a3f1",
    "label": "host",
    "project_id": "AAA-111-...",
    "project_path": "/Users/x/Work/MyProject.uproject",
    "engine_version": "5.4.2",
    "session_id": "9f2e",
    "pid": 12345,
    "started_at": "2026-04-27T14:32:00Z"
  },
  "asset_registry_hash": "sha256:..."
}
```

## Tool Catalog

Non-exhaustive list. Items grouped by domain.

### Discovery

| Tool | Purpose |
|---|---|
| `list_editors()` | Connected editor instances |
| `get_editor(id_or_label)` | Editor details |
| `get_active_editor()` | Currently active editor for this session |
| `set_active_editor(id_or_label)` | Switch active editor |
| `wait_for_editor(slot_id?, timeout_ms?)` | Block until plugin handshake completes (or matching slot_id arrives). Replaces post-`restart_editor` polling: condition_variable signaled on `hello`, returns the instant the editor connects. Already-connected editors return immediately with `already_connected=true`. Timeout returns `EditorNotConnected` (-32001). Default 120000ms, max 600000ms. |

### Indexing

| Tool | Purpose |
|---|---|
| `get_indexing_status(slot?)` | Tier-by-tier progress |
| `trigger_reindex(slot, scope, paths?)` | Manual rescan |
| `mark_paths_for_deep_index(paths)` | Promote to T3 eager |
| `configure_indexing(throttle_pct, parallelism)` | Runtime tuning |

### Slot Management

| Tool | Purpose |
|---|---|
| `list_slots()` | All known slots |
| `merge_slots(source, target)` | Combine duplicate-ProjectID slots |
| `migrate_slot(from, to)` | Rename slot when project moves |
| `prune_slots(before)` | Cleanup orphaned slots |

### Modification (Atomic)

| Tool | Purpose |
|---|---|
| `modify_actor_property(actor, prop, value, _expected_version?)` | Single property change |
| `spawn_actor(bp_or_class, location, rotation?, _label?)` | Add actor |
| `delete_actor(actor)` | Remove actor |
| `modify_asset_property(asset, prop, value, _expected_version?)` | Asset-level change |
| `add_component(actor, component_class)` | Component add |
| `remove_component(actor, component_id)` | Component remove |
| `compare_and_set_property(target, prop, expected, new)` | Atomic CAS |

### Modification (Multi-step)

| Tool | Purpose |
|---|---|
| `begin_transaction(label) → tx_id` | Start grouped operation |
| `commit_transaction(tx_id)` | Finalize |
| `rollback_transaction(tx_id)` | Cancel |
| `get_active_transactions()` | Open multi-step list |

### Modification (Bulk)

| Tool | Purpose |
|---|---|
| `bulk_modify(operations, _atomic?)` | Batch ops; atomic by default |

### Save & Dirty

| Tool | Purpose |
|---|---|
| `save_assets(paths?, dry_run?)` | Persist dirty assets |
| `get_dirty_assets()` | List unsaved |
| `discard_changes(paths)` | Revert dirty to disk state |

### Compile

| Tool | Purpose |
|---|---|
| `compile_and_reload(target, _strategy?, _editors?, _on_failure?)` | Compile + reload (Live Coding or full restart) |
| `analyze_change(diff_or_paths)` | Pre-flight strategy recommendation |
| `get_live_coding_status(_editor?)` | LC availability + patch count |

### Audit

| Tool | Purpose |
|---|---|
| `get_transaction_history(slot, since?, limit?, filter?)` | Audit log query |
| `inspect_transaction(tx_id)` | Full transaction detail |
| `revert_transaction(tx_id)` | Compensating revert |

### Knowledge Graph (3-Layer API)

3-layer architecture per [ADR-011](../decisions/adr-011-query-dsl.md): high-level tools, Cypher subset for advanced queries, mutation tools separately.

**Layer 1 — High-level tools (token-cheap, sandboxed)**

| Tool | Purpose |
|---|---|
| `impact_of(asset_or_class, _max_depth?)` | What depends on this? Reverse `depends_on` traversal |
| `references_to(target, _kind?)` | Inbound references (hard / soft / redirector) |
| `class_hierarchy(class, _direction?)` | UClass parent / child tree |
| `find_by_class(class_pattern, _module?)` | Asset / instance enumeration |
| `find_unused(asset_kind?)` | Assets with no `depends_on` incoming |

**Layer 2 — Cypher subset (advanced)**

| Tool | Purpose |
|---|---|
| `query(cypher, params?, _max_depth?, _no_cap?)` | Custom graph query — read-only Cypher subset |

Cypher subset rules:
- ✅ `MATCH`, `RETURN`, `WHERE`, `WITH`, `ORDER BY`, `LIMIT`, `SKIP`, `OPTIONAL MATCH`, aggregation (`COUNT`, `SUM`, `AVG`, `COLLECT`), parameters (`$param`)
- ❌ `CREATE`, `DELETE`, `SET`, `MERGE`, `REMOVE`, `CALL`, unbounded `*`, multi-statement
- Bounded traversal `*1..N` mandatory: default max `*1..5`, override max `*1..10`
- Result auto-truncated at ~8K tokens with `refine_hint`. `_no_cap: true` to override

**Layer 3 — Mutation tools** (listed separately in `Modification` sections above): `modify_actor_property`, `spawn_actor`, `delete_actor`, `bulk_modify`, vb. Cypher mutation operatörleri yasak — mutation tools üzerinden auditable + transactional.

## Token Optimization Principles

See [ADR-007](../decisions/adr-007-token-optimization.md) for full rationale. Applied design-time across all tool APIs:

1. **Schema-on-demand** — `verbose: true` opens advanced parameters
2. **Field selection** — `fields: ["name", "type"]` constrains response
3. **Pagination + cursor** — list tools default limit 50
4. **ID-first responses** — return refs, expand separately
5. **Smart truncation** — `…` with `show_full(ref)` to retrieve
6. **Tier-aware queries** — T1 satisfies most; T2/T3 omitted unless requested
7. **Streaming** — long-running outputs over SSE chunks
8. **Hard cap** — ~8K tokens; auto-truncate + `refine_query` hint
9. **Compact encoding** — asset paths to numeric IDs in queries
10. **Query DSL** — Cypher subset; only requested traversal returns

## Error Codes

JSON-RPC 2.0 standard codes plus Sage-specific:

| Code | Meaning |
|---|---|
| -32600 | Invalid request |
| -32602 | Invalid params |
| -32603 | Internal error |
| -32000 | Generic Sage error |
| -32001 | Editor not connected |
| -32002 | Slot not found |
| -32003 | Version conflict (optimistic lock fail) |
| -32004 | PIE active, modification rejected |
| -32005 | Source control checkout required |
| -32006 | Compile error |
| -32007 | Live Coding unavailable |
| -32008 | Hard cap exceeded, refine query |
