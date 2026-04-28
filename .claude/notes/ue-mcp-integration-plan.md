# UE-MCP → Sage Integration Roadmap

> **Source:** `/Users/mahmutalemdar/Developer/alemdarlabs/ue-mcp` (TypeScript MCP server + C++ plugin, 562 actions across 19 categories, BUSL-1.1 → Apache after 4 yr).
> **Target:** Sage Unreal MCP — currently 51 tools (Phase 1 + 2 + 3 partial).
> **Strategy:** Identify capability gaps, prioritise integration in tiers, decide what to skip.

---

## License & Architecture Posture

ue-mcp is **BUSL-1.1** — non-compete clause prevents cloning the project's identity but does not block re-implementing capabilities (which is what we're doing). We don't fork code — we read it as a capability spec, then build native sage-bridge handlers in our own architecture (C++23 server, no TypeScript stdio).

ue-mcp's **architectural choices we will NOT adopt:**
- TypeScript stdio MCP server (we're C++23 HTTP+SSE — better for multi-editor, durable sessions)
- 562 actions in 19 mega-tools (we expose distinct MCP tools — easier for the agent to discover via `tools/list`)
- 30s sync RPC timeout per request (we already have async event channel for streamed deltas)
- Flow YAML executor (premature; LLM agents already orchestrate)

ue-mcp's **architectural choices we WILL adopt:**
- HandlerRegistry pattern for plugin-side tool dispatch — we already have `FSageToolDispatch`
- GameThreadExecutor — we already have `RunOnGameThread<>` template
- JSON ↔ UProperty reflection coverage including TArray/TMap/TObjectPtr/TSubclassOf — we have primitives, missing collections (Phase 4.0 below)
- SQLite FTS5 over assets — fits our SQLite slot store (Phase 4.x)

---

## Capability Gap Map

### 🟢 Already covered by Sage (overlap)

| ue-mcp action | Sage equivalent |
|---|---|
| editor.spawn_actor | `spawn_actor` |
| editor.delete_actor | `delete_actor` |
| editor.set_actor_property | `modify_actor_property` |
| asset.duplicate / rename / delete | same |
| asset.save | `save_assets` |
| level.save | `save_level` |
| editor.toggle_pie | `run_pie` / `stop_pie` |
| material.set_parameter | `modify_material_parameter` |
| asset.get_referencers | `references_to` (richer — depth-bounded) |
| asset.get_dependencies | `query_graph` (any traversal) |
| build.compile_plugin | `restart_editor(build_plugin=true)` |
| reflection.* | partial — no `reflect_class` / `reflect_struct` yet |

ue-mcp's reverse-deps + dependency lookups are **inferior** to our impact_of/references_to — we have Kuzu-backed traversal, they walk the asset registry every call.

### 🔴 Genuinely missing from Sage — high value

These would meaningfully expand what an agent can do in UE through Sage. Grouped by tier.

---

## Tier A — Quick Wins (≤1 day each, native to existing Sage subsystems)

| # | Tool | Source category | Why valuable | Effort |
|---|---|---|---|---|
| A1 | `reflect_class(class_path)` | reflection | "What properties does this UClass have?" — schema introspection. Pure UE reflection walk. | S |
| A2 | `reflect_struct(struct_path)` | reflection | Same for USTRUCT. | S |
| A3 | `reflect_enum(enum_path)` | reflection | Same for UENUM. | S |
| A4 | `list_classes(filter?)` | reflection | Class registry listing. Sage already populates the Class node — this is just an MCP-tool wrapper around a Cypher query. | S |
| A5 | `get_mesh_bounds(asset_path)` | asset | Static/skeletal mesh bbox + extent. Common Q for placement. | S |
| A6 | `get_mesh_collision(asset_path)` | asset | Collision primitive count + names. | S |
| A7 | `get_referencers(asset_path)` | asset | Already covered by `references_to` — confirm parity, document overlap. | — |
| A8 | `bulk_rename_assets([{src,dst}])` | asset | Single FScopedTransaction across N renames — extends `rename_asset`. | S |
| A9 | `move_folder(src, dst)` | asset | Folder-level move with redirector fixup. | S |
| A10 | `create_gameplay_tag(name)` | gameplay | Editor-side ini write; tag tree update. | S |
| A11 | `list_gameplay_tags(filter?)` | gameplay | Hierarchical tag listing. | S |
| A12 | `set_dialog_policy(policy)` | misc | Auto-respond modal dialogs (critical for headless ops — UE prompts during long ops). | M |
| A13 | `respond_to_dialog(id, choice)` | misc | Manual dialog response. | S |
| A14 | `list_dialogs()` | misc | Pending dialog poll. | S |
| A15 | `set_world_setting(key, value)` | level | World settings UProperty. | S |

**Tier A total: 15 tools, ~3-5 days work. Doubles the discoverable surface for read-side intel.**

---

## Tier B — Blueprint Introspection (1-2 weeks)

The single biggest gap. ue-mcp has 49 Blueprint actions; Sage has zero.

| # | Tool | Notes |
|---|---|---|
| B1 | `bp.read(path)` | Top-level summary: parent class, components, var count, fn count, interfaces |
| B2 | `bp.list_variables(path)` | All variables: name, type, default, access |
| B3 | `bp.list_functions(path)` | All functions: name, signature, pure flag, access |
| B4 | `bp.read_function_graph(path, fn_name)` | Node list + connections — agent reads BP logic |
| B5 | `bp.get_execution_flow(path, fn_name)` | BFS traversal of exec pins from entry — easier than raw graph |
| B6 | `bp.read_components(path)` | SCS tree: name, class, parent, default props |
| B7 | `bp.list_node_types(filter?)` | Available BP nodes (palette query) |
| B8 | `bp.search_nodes(path, kw)` | "Find all nodes mentioning X" |
| B9 | `bp.compile(path)` | Trigger compile, return result + errors |
| B10 | `bp.validate(path)` | Compile-without-output dry run |

Plugin needs new handler group (`SageBlueprintTools.cpp`). Reflection-heavy: `UBlueprint`, `UEdGraph`, `UK2Node` traversal.

**Tier B total: 10 tools (read-only). Write-side BP authoring (B11+) is Tier D — much harder.**

---

## Tier C — Material Graph (3-5 days, focused)

Sage has 1 material tool (`modify_material_parameter`). ue-mcp has 36, including full graph authoring.

| # | Tool | Notes |
|---|---|---|
| C1 | `mat.read(path)` | Material/instance summary: domain, blend mode, parameter list |
| C2 | `mat.list_parameters(path)` | Scalar/vector/texture params with defaults |
| C3 | `mat.list_expressions(path)` | Graph node list — class, position, connections |
| C4 | `mat.create_instance(parent, dest)` | UMaterialInstanceConstant from a parent |
| C5 | `mat.set_expression_value(path, node_id, value)` | Edit a constant in the graph |
| C6 | `mat.get_shader_stats(path)` | Texture sample counts, instruction counts |

Skip for now: `build_graph` (declarative graph spec) and `render_preview` — both heavy and niche.

---

## Tier D — Heavy Lift (each is its own multi-week milestone)

These deserve standalone phases when there's clear demand.

| # | Subsystem | ue-mcp count | Why heavy |
|---|---|---|---|
| D1 | Animation (AnimBP, montage, IK Rig, ControlRig, blendspace) | 56 | New plugin handler group; UE animation API is large surface |
| D2 | Niagara (system, emitter, HLSL modules, GPU shader inspect) | 37 | Editor-only API, requires NiagaraEditor module link |
| D3 | AI / Gameplay framework (BT, EQS, StateTree, SmartObject, Perception) | 35 (subset of gameplay 59) | Multiple separate UE modules |
| D4 | Enhanced Input system (IMC, modifiers, triggers) | ~10 | Recent UE5 surface, less mature reflection |
| D5 | PCG (Procedural Content Generation graphs) | 23 | Brand new UE 5.4+ subsystem; volatile API |
| D6 | Landscape (sculpt, paint, splines, heightmap) | 14 | Editor-only, needs LandscapeEditor module |
| D7 | Foliage (types, painting, instances) | 13 | Same — FoliageEdit module |
| D8 | GAS (Gameplay Ability System) | 18 | Plugin module — only useful if project uses GAS |
| D9 | Networking (replication, dormancy, relevancy) | 14 | UProperty introspection — cheap to extend reflect_class |
| D10 | Widget/UMG | 21 | UMG editor module — uncommon for AI agent use |

**Recommendation:** Land Tier A + B + C first (~3 weeks). Tier D items only when a concrete user need arrives — speculative coverage of 562 actions burns engineering time on capabilities no one will discover.

---

## Tier E — Things Sage Already Does Better

| Sage capability | ue-mcp counterpart | Why Sage wins |
|---|---|---|
| `impact_of(asset, depth)` | asset.get_referencers | Bounded transitive traversal, Kuzu-backed; theirs walks AssetRegistry per call |
| `references_to(asset)` | same | sub-ms response from cached graph |
| `find_unused()` | none equivalent | Pure-graph Cypher; requires no scan |
| `query_graph(cypher)` | none | Read-only Cypher subset with safety |
| `class_hierarchy(class)` | reflection.list_classes | Theirs gives flat list; ours walks INHERITS_FROM tree |
| `restart_editor` orchestrator | build.compile_plugin (Win-only LC) | Cross-platform full restart with handshake confirmation |
| Real-time AssetRegistry delta | none | Live graph patches; ue-mcp re-scans on every query |
| Slot-scoped per-editor isolation | none | ue-mcp is single-project per server instance |
| `compare_and_set_property` | none | Optimistic concurrency for multi-step plans |
| `bulk_modify(atomic=true)` | flows engine | First-class atomic multi-op without YAML scripting |

The intelligence layer (Phase 2 graph + Phase 3 reflection) is **the differentiator**; everything else is execution-layer parity work.

---

## Skip Outright

| Item | Reason |
|---|---|
| ue-mcp `feedback` tool (auto GitHub issue) | Their workflow, not ours. We capture lessons in `.claude/notes/lessons.md`. |
| `flow` (YAML multi-step executor) | LLM is already the orchestrator; YAML adds an intermediate language. |
| `editor.run_python` | We're not embedding Python; agent already has Python via host environment. |
| `console_command(cmd)` | Footgun — UE console can do anything including crash. Add behind explicit-confirm flag if needed. |
| `demo.build_neon_shrine` | Project-specific marketing demo; not generic. |

---

## Concrete Task Breakdown — Phase 4 Plan

**Phase 4.0 — UProperty collection support** (groundwork)
- TArray ↔ JSON array round-trip in `set_property` family
- TMap ↔ JSON object
- TObjectPtr / TSoftObjectPtr asset reference resolution
- TSubclassOf class path resolution
- Unblocks B1-B10 + many later Tier D items
- _Effort: ~2 days. Lands as a `SageToolHelpers` extension._

**Phase 4.1 — Tier A reflection + asset extras** (15 tools)
- A1-A4: reflect_class/struct/enum/list_classes — read-only Cypher + UE reflection
- A5-A6: get_mesh_bounds / get_mesh_collision
- A8-A9: bulk_rename_assets / move_folder
- A10-A11: gameplay tag CRUD
- A12-A14: dialog policy + auto-respond
- _Effort: ~3 days. One commit per tool group._

**Phase 4.2 — Blueprint read** (10 tools, B1-B10)
- New plugin handler `SageBlueprintTools.cpp`
- New server-side schemas + tool registrations (or main.cpp inline)
- Smoke harness with a fixture .uasset (BP_ThirdPersonCharacter)
- _Effort: ~5 days. Read-only — no graph mutation yet._

**Phase 4.3 — Material graph authoring** (6 tools, C1-C6)
- New plugin handler `SageMaterialGraphTools.cpp` (separate from existing `SageMaterialTools.cpp`)
- _Effort: ~4 days. Skip declarative builder + preview render in this pass._

**Phase 4.4 — Index perf via Kuzu COPY FROM JSON** (transverse)
- ingestSnapshot writes temp JSON files, runs COPY FROM
- 8K asset / 16K dep / 8K class current ~30s → target <5s
- _Effort: ~2 days. Important before Phase 4.2 lands more class metadata._

**Phase 4.5 — UE Editor headless / no-UI test** (transverse)
- Currently any plugin test requires `open <uproject>` + 20s boot
- ue-mcp has filesystem-only fallbacks for INI/header parsing
- Replicate the pattern: a "lite" plugin handler set that operates on disk assets without an editor world
- _Effort: ~3 days. Doubles smoke iteration speed._

---

## Phase 4 commit-tree shape

```
Phase 4 — Execution + reflection parity (Tier A + B + C, ~3 weeks)
├── 4.0 UProperty collections (1 commit)
├── 4.1 Tier A reflection & asset extras (5 commits)
│     ├── 4.1.1 reflect_class/struct/enum + list_classes
│     ├── 4.1.2 get_mesh_bounds + get_mesh_collision
│     ├── 4.1.3 bulk_rename_assets + move_folder
│     ├── 4.1.4 gameplay tags CRUD
│     └── 4.1.5 dialog policy
├── 4.2 Blueprint read (3 commits)
│     ├── 4.2.1 read + list_variables + list_functions
│     ├── 4.2.2 read_graph + execution_flow
│     └── 4.2.3 list_node_types + search_nodes + validate + compile
├── 4.3 Material graph authoring (2 commits)
│     ├── 4.3.1 read + list_parameters + list_expressions + shader_stats
│     └── 4.3.2 create_instance + set_expression_value
├── 4.4 Kuzu COPY FROM perf (1 commit)
└── 4.5 Headless test mode (1 commit)
```

**Total: 13 commits, ~50 new tools (51 → ~101).** Brings Sage into rough functional parity with ue-mcp's most-used surface while keeping the knowledge-graph moat intact.

Phase 5 (Tier D heavy lifts — Animation, Niagara, AI, PCG, Landscape, Foliage, GAS) only on demand.

---

## Open questions for the user

1. **Blueprint write tools (Tier D'ish)?** Read is in 4.2; full graph authoring (add_node, connect_pins, T3D round-trip) is a separate ~1 week. Defer or include?
2. **Gameplay tag system priority?** A10-A11 are quick wins but only useful if your projects use GAS-style tagging.
3. **GitHub issue feedback loop (ue-mcp's `feedback` tool)?** Sage's `.claude/notes/lessons.md` covers the same ground but isn't agent-visible. Worth wiring an MCP `report_issue` tool that writes there?
4. **Cross-platform scope:** Tier D Niagara/Material editing depend on UE editor modules. Confirmed Mac+Linux+Win all targets, or Mac/Win only?
