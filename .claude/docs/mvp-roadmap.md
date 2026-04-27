# MVP Roadmap

> Status: Active. Tracks Phase 1 (Execution Full) and Phase 2 (Knowledge Layer) milestones.
> Last updated: 2026-04-27. See [ADR-012](../decisions/adr-012-mvp-scope.md) for the canonical sequencing decision.

## Phase 1 — Execution Full (4-6 weeks)

**Goal state:** Sage agent has comprehensive editor control. Knowledge graph minimal (slot identity + skeletal `AssetRegistry` sync only). Demo: "AI projeyi tam yönetiyor."

### Milestone 1.1 — Server Scaffolding (~1 week)
- CMake workspace + vcpkg manifest mode
- C++23 baseline, clang-format / clang-tidy config
- Manual MCP impl skeleton (JSON-RPC envelope, capability handshake, tool registration framework)
- HTTP+SSE transport server
- spdlog + Catch2 + ASan/UBSan setup

### Milestone 1.2 — Plugin Scaffolding (~1 week)
- UPlugin structure (`SageBridge.uplugin`, `Source/SageBridge/`)
- `FWebSocketsModule` client + reconnect with exponential backoff
- Heartbeat + handshake (15s interval)
- Editor identity (slot_id formula per ADR-003)
- Multi-editor labels (CLI argument + per-instance config)
- Build pipeline: `BuildPlugin.bat` packaging

### Milestone 1.3 — Core Mutation Tools (~1 week)
- Actor: `spawn_actor`, `delete_actor`, `modify_actor_property`, `set_transform`, `set_visibility`
- Component: `add_component`, `remove_component`, `modify_component_property`, `attach`, `detach`
- Asset: `modify_asset_property`, `rename_asset`, `move_asset`, `duplicate_asset`, `delete_asset`
- Each wrapped in `FScopedTransaction` (per ADR-006)

### Milestone 1.4 — Transaction Layer (~1 week)
- `begin_transaction` / `commit_transaction` / `rollback_transaction` multi-step
- `compare_and_set_property`, `_expected_version` optimistic locking
- `bulk_modify` atomic-by-default
- Audit log: SQLite tables per database-schema.md

### Milestone 1.5 — Multi-editor + Lifecycle (~1 week)
- `list_editors`, `get_active_editor`, `set_active_editor`, `_editor` per-tool override
- Slot conflict resolution: `merge_slots`, `migrate_slot`, `prune_slots`
- Connection state machine: CONNECTING → HANDSHAKING → CONNECTED → EDITOR_LOST → RESTARTING/EDITOR_DEAD
- Operation queue persisted in SQLite, replay on reconnect
- State snapshot + restore (open assets, optional selection)

### Milestone 1.6 — Compile Coordination (~1 week)
- `compile_and_reload(target, _strategy?, _editors?, _on_failure?)` per ADR-009
- `AnalyzeChanges()` regex-based reflection diff
- Live Coding integration (`ILiveCodingModule::Compile`)
- Full restart orchestration: save → shutdown → UBT subprocess → relaunch → reconnect
- `analyze_change` (pre-flight strategy recommendation)
- Multi-editor LC escalation policy (default FullRestart for shared modules)
- `get_live_coding_status` with patch fragmentation tracking

### Milestone 1.7 — PIE + Tests + Source Control (~1 week, optional buffer)
- `run_pie`, `stop_pie`, `set_play_mode`
- `run_tests` via Automation Framework
- `save_assets` with auto-checkout via `ISourceControlModule`
- PIE policy enforcement (hard reject mutations unless `_allow_pie: true`)

**Phase 1 Exit Criteria:**
- All listed tools functional
- Multi-editor scenario tested (host + client + experiment)
- Compile cycle (Live Coding + full restart) verified end-to-end
- Audit log populated, state survives editor restart
- **Demo 1:** "Tell AI to do X, X happens — across multiple editors, with undo."

## Phase 2 — Knowledge Layer (4-6 weeks)

**Goal state:** AI understands + controls. Demo: Sage moat proven.

### Milestone 2.1 — KuzuDB Integration (~1 week)
- KuzuDB embedded init, slot-scoped database files
- `GraphStore` trait + `KuzuGraphStore` impl
- Schema migration framework (forward-only, `_SchemaVersion` node)
- Cypher query proxy with subset validator (per ADR-011)

### Milestone 2.2 — T1 Indexing (~1 week)
- `AssetRegistry` full scan on connect
- T1 nodes: `Asset`, `Class`, `Module`, `Plugin` (metadata only)
- Worker thread pool with throttle controller (CPU < 30%, frame time < 16ms per ADR-005)
- Resumable indexing state in KuzuDB
- Progress streaming to client (SSE chunks)

### Milestone 2.3 — T2 Indexing + Real-Time Delta (~1 week)
- T2 topology pass: `depends_on`, `inherits_from`, `implements`, `lives_in`, `contains` edges
- AssetRegistry event subscription: `OnAssetAdded/Updated/Removed/Renamed`
- Real-time delta sync (target: 50 events/sec real-time, batch above)
- AssetRegistry hash-based reconnect optimization (delta-only resync)

### Milestone 2.4 — High-Level Query Tools (~1 week)
- `impact_of(asset_or_class, _max_depth?)` — reverse `depends_on` traversal
- `references_to(target, _kind?)` — inbound `references` filtered by hard/soft/redirector
- `class_hierarchy(class, _direction?)` — recursive `inherits_from`
- `find_by_class(pattern, _module?)`, `find_unused(asset_kind?)`
- Tier-aware response shaping (T1 satisfies most; T2/T3 only if requested)
- Pagination + cursor + 8K token cap (per ADR-007)

### Milestone 2.5 — Cypher Subset Layer 2 (~1 week)
- KuzuDB parser AST whitelist walk
- Mutation operator rejection (`CREATE`/`DELETE`/`SET`/`MERGE`/`REMOVE`)
- Bounded `*1..N` enforcement (max 5 default, 10 override)
- `query(cypher, params?, _max_depth?, _no_cap?)` tool implementation
- Result truncation + `refine_hint`

**Phase 2 Exit Criteria:**
- Indexing 50K-asset project T1+T2 in <6 minutes
- `impact_of` and `references_to` accurate against ground-truth (UE Reference Viewer comparison)
- Cypher subset rejects all forbidden constructs
- Real-time delta keeps graph in sync during live editing
- **Demo 2:** "AI: 'BP_Enemy'yi silmeden önce neyi etkileyeceğine bak.' → real impact analysis → 'sil' → atomic transaction → undo'da."

## Out of Scope (V2)

These are explicitly deferred and tracked in [ADR-010 Open Questions](../decisions/adr-010-schema-design.md):

- BP K2Node graph indexing (Blueprint exec pin routines)
- Collapsed function reflection access
- Macro library (`UBlueprintMacroLibrary`) representation
- Event graph specialization (`(:Event)` node type)
- Clang AST reflection diff (V1: regex-based, V2: clang AST)
- ActorRef T2 eager indexing (V1: lazy on level open)

## Risk Register

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| Phase 1 scope creep (tool catalog grows beyond budget) | High | Schedule slip | Tight discipline; out-of-scope listed; reviewer challenge per milestone |
| KuzuDB community small, hit blocker bug | Medium | Phase 2 delay | `GraphStore` trait abstraction; SQLite fallback path planned |
| Live Coding integration flaky on edge cases | Medium | Stability | Probe + escalate to FullRestart per ADR-009 |
| `AssetRegistry` event firehose during bulk imports | Low | UI lag | Throttle + batch policies, defer to next frame if backed up |
| Phase 1 → Phase 2 refactor cost | Medium | Schedule slip | Phase 1 establishes event publishing hooks; Phase 2 plugs in subscribers |
| Phase 1 demo lacks differentiation (commodity) | High | Marketing | Accept; Phase 2 is the differentiation milestone, communicate roadmap clearly |
| Multi-editor compile coordination edge cases | Medium | UX glitches | Default to safest policy (FullRestart escalation); power-user override flag |

## Status

**Current:** Pre-implementation. Architecture design complete (ADR-001..012). Ready to start Phase 1 Milestone 1.1.

## Next Action

Start Milestone 1.1 — Server Scaffolding. CMake workspace + vcpkg manifest + MCP impl skeleton.
