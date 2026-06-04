# MVP Roadmap — TAMAMLANDI

> Note: ADR-018 retired the KuzuDB-backed Phase 2 graph layer. This file keeps historical roadmap context; active build/runtime scope must follow ADR-018.

> **Status: Phase 1 + Phase 2 + Phase 3 + Phase 4 COMPLETE** (2026-04-28 · 82 commit · 443 tool)
> Bu belge orijinal planı kayıt altına alır. Her milestone tamamlanmıştır.
> Kanıtlama: `git log --oneline` · `grep -rh RegisterHandler plugin/.../Tools/ | wc -l`
> Bkz. [ADR-012](../decisions/adr-012-mvp-scope.md) — sequencing kararı, doğrulandı.

## Phase 1 — Execution Full ✓ TAMAMLANDI

**Gerçek süre:** ~1 hafta. **Hedef durum ulaşıldı.**

### Milestone 1.1 — Server Scaffolding ✓
- CMake workspace + vcpkg manifest mode
- C++23 baseline, clang-format / clang-tidy config
- Manual MCP impl skeleton (JSON-RPC envelope, capability handshake, tool registration framework)
- HTTP+SSE transport server
- spdlog + Catch2 + ASan/UBSan setup

### Milestone 1.2 — Plugin Scaffolding ✓
- UPlugin structure (`SageBridge.uplugin`, `Source/SageBridge/`)
- `FWebSocketsModule` client + reconnect with exponential backoff
- Heartbeat + handshake (15s interval)
- Editor identity (slot_id formula per ADR-003)
- Multi-editor labels (CLI argument + per-instance config)
- Build pipeline: `BuildPlugin.bat` packaging

### Milestone 1.3 — Core Mutation Tools ✓
- Actor: `spawn_actor`, `delete_actor`, `modify_actor_property`, `set_transform`, `set_visibility`
- Component: `add_component`, `remove_component`, `modify_component_property`, `attach`, `detach`
- Asset: `modify_asset_property`, `rename_asset`, `move_asset`, `duplicate_asset`, `delete_asset`
- Each wrapped in `FScopedTransaction` (per ADR-006)

### Milestone 1.4 — Transaction Layer ✓
- `begin_transaction` / `commit_transaction` / `rollback_transaction` multi-step
- `compare_and_set_property`, `_expected_version` optimistic locking
- `bulk_modify` atomic-by-default
- Audit log: SQLite tables per database-schema.md

### Milestone 1.5 — Multi-editor + Lifecycle ✓
- `list_editors`, `get_active_editor`, `set_active_editor`, `_editor` per-tool override
- Slot conflict resolution: `merge_slots`, `migrate_slot`, `prune_slots`
- Connection state machine: CONNECTING → HANDSHAKING → CONNECTED → EDITOR_LOST → RESTARTING/EDITOR_DEAD
- Operation queue persisted in SQLite, replay on reconnect
- State snapshot + restore (open assets, optional selection)

### Milestone 1.6 — Compile Coordination ✓
- `compile_and_reload(target, _strategy?, _editors?, _on_failure?)` per ADR-009
- `AnalyzeChanges()` regex-based reflection diff
- Live Coding integration (`ILiveCodingModule::Compile`)
- Full restart orchestration: save → shutdown → UBT subprocess → relaunch → reconnect
- `analyze_change` (pre-flight strategy recommendation)
- Multi-editor LC escalation policy (default FullRestart for shared modules)
- `get_live_coding_status` with patch fragmentation tracking

### Milestone 1.7 — PIE + Tests + Source Control ✓
- `run_pie`, `stop_pie`, `set_play_mode`
- `run_tests` via Automation Framework
- `save_assets` with auto-checkout via `ISourceControlModule`
- PIE policy enforcement (hard reject mutations unless `_allow_pie: true`)

**Phase 1 Exit Criteria:** ✓ TÜM SAĞLANDI

## Phase 2 - Retired Knowledge Layer (superseded by ADR-018)

**Gerçek süre:** ~1 hafta. **Hedef durum ulaşıldı.**

### Milestone 2.1 - Retired KuzuDB Integration (superseded by ADR-018)
- KuzuDB embedded init, slot-scoped database files
- `GraphStore` trait + `KuzuGraphStore` impl
- Schema migration framework (forward-only, `_SchemaVersion` node)
- Cypher query proxy with subset validator (per ADR-011)

### Milestone 2.2 — T1 Indexing ✓
- `AssetRegistry` full scan on connect
- T1 nodes: `Asset`, `Class`, `Module`, `Plugin` (metadata only)
- Worker thread pool with throttle controller (CPU < 30%, frame time < 16ms per ADR-005)
- Resumable indexing state in KuzuDB
- Progress streaming to client (SSE chunks)

### Milestone 2.3 — T2 Indexing + Real-Time Delta ✓
- T2 topology pass: `depends_on`, `inherits_from`, `implements`, `lives_in`, `contains` edges
- AssetRegistry event subscription: `OnAssetAdded/Updated/Removed/Renamed`
- Real-time delta sync (target: 50 events/sec real-time, batch above)
- AssetRegistry hash-based reconnect optimization (delta-only resync)

### Milestone 2.4 — High-Level Query Tools ✓
- `impact_of(asset_or_class, _max_depth?)` — reverse `depends_on` traversal
- `references_to(target, _kind?)` — inbound `references` filtered by hard/soft/redirector
- `class_hierarchy(class, _direction?)` — recursive `inherits_from`
- `find_by_class(pattern, _module?)`, `find_unused(asset_kind?)`
- Tier-aware response shaping (T1 satisfies most; T2/T3 only if requested)
- Pagination + cursor + 8K token cap (per ADR-007)

### Milestone 2.5 - Retired Cypher Subset Layer 2 (superseded by ADR-018)
- KuzuDB parser AST whitelist walk
- Mutation operator rejection (`CREATE`/`DELETE`/`SET`/`MERGE`/`REMOVE`)
- Bounded `*1..N` enforcement (max 5 default, 10 override)
- `query(cypher, params?, _max_depth?, _no_cap?)` tool implementation
- Result truncation + `refine_hint`

**Phase 2 Exit Criteria:** ✓ TÜM SAĞLANDI
- Indexing verified on SageTest: 8 359 asset · 16 093 DEPENDS_ON · 8 337 UClass
- Real-time delta: asset_added / asset_removed / asset_renamed events live
- Cypher subset: mutation operators reddediliyor, *N..M sınırlı

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

## Gerçekleşen Durum (2026-04-28)

**Phase 1 + 2 + 3 + 4 COMPLETE.**

- 82 commit, main branch
- Plugin: 443 tool handler (32 .cpp dosyası)
- Server: 454 tool şeması (`tools/list` tam)
- UE-MCP parity: 445 / 448 (%99.3) — 3 N/A (feedback + demo kategorileri)
- Her iki binary build clean: plugin dylib + sage-server

## Phase 4 Tamamlanan Domain'ler

Phase 4 orijinal planda yoktu; UE-MCP (448 action) ile tam pariteye ulaşmak için eklendi.

| Domain | Tool sayısı |
|---|---:|
| animation | 46 |
| gameplay | 45 |
| niagara | 26 |
| level (extension) | 22 |
| blueprint (extension) | 34 |
| asset (extension) | 25 |
| editor (extension) | 18 |
| pcg | 16 |
| project (extension) | 13 |
| landscape | 11 |
| networking | 11 |
| material (extension) | 13 |
| widget (extension) | 11 |
| gas | 9 |
| foliage | 7 |
| audio | 5 |
| reflection (extension) | 4 |
| sequencer (extension) | 3 |

## Sonraki Adımlar

Bkz. [`.claude/notes/ue-mcp-tasks.md`](../notes/ue-mcp-tasks.md) — 3 N/A item dışında tüm backlog tamamlandı.

Olası Phase 5 konuları:
- End-to-end entegrasyon test suite (Catch2, gerçek UE editor)
- Smoke test script'leri her domain için
- `sage.toml` config dosyası (env var override'ların altına)
- `docs/` (public-facing) başlatılması
