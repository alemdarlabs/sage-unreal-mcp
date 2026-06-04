# Niagara MCP Reference Matrix

Date: 2026-06-04

Purpose: source-backed comparison for the 2026-06-03 Niagara gap sweep. This is not a copy target; it is a parity and product-positioning checklist for Sage's own Niagara surface.

Local audit copies were cloned under:

```text
C:\Users\mahmu\AppData\Local\Temp\sage-ref-audit-20260604-004937
```

## Public References Checked

| Project | Source | License / distribution | Verified Niagara signal | Sage takeaway |
|---|---|---|---|---|
| Monolith | https://github.com/tumourlove/monolith and https://github.com/tumourlove/monolith/wiki/Tool-Reference | MIT; public GitHub repo | Tool Reference lists `niagara_query` with 129 actions. The v0.18 docs call out direct `CustomHlsl` node read/write, HLSL module/function creation, dynamic-input lifecycle, sim-stage/event-handler selectors, event/sim-stage CRUD, renderer management, temporal-control composite writers, stateless-emitter factory, search/discovery pack, validation, and `audit_cross_asset_refs`. | Strongest Niagara benchmark. Sage should not copy MIT code, but the feature taxonomy is the best public parity contract. |
| db-lyon UE-MCP | https://github.com/db-lyon/ue-mcp and https://db-lyon.github.io/ue-mcp/tool-reference/ | Public GitHub repo; docs expose 21 category tools and 500+ actions. | `niagara` category exposes 26 actions: systems, emitters, spawn, runtime parameter set, renderer CRUD, data-interface inspection, system-from-spec, compiled HLSL, module inputs/static switches, HLSL module creation, scratch module creation, batch. Local clone confirms `src/tools/niagara.ts` maps these to C++ bridge handlers. | Closest ergonomic reference to Sage's explicit-tool style. Sage now exceeds breadth in collections, sim cache, dependency audit, preview lifecycle, and scalability, but is behind on real HLSL module/scratch creation. |
| ChiR24/Unreal_mcp | https://github.com/ChiR24/Unreal_mcp | MIT; public GitHub repo; NPM-style distribution. | README claims Visual Effects and Niagara graph manipulation. Local `docs/handler-mapping.md` lists Phase 12 Niagara authoring actions for spawn-rate/burst/per-unit modules, initialize/force/velocity/size/color/collision/kill/camera-offset modules, renderer presets, data-interface adders, event generator/receiver, GPU sim enable, simulation stage, validation. | Useful preset/action taxonomy. Even if implementation quality varies, the preset module actions are agent-friendly and reduce raw graph editing burden. |
| Flopperam Unreal Engine MCP | https://github.com/flopperam/unreal-engine-mcp | Split model: hosted commercial surface plus simpler local open-source repo. | README/skills name hosted VFX tools `niagara_inspect`, `niagara_edit`, and `niagara_script_edit`; local repo is explicitly a simpler community subset. | Commercial positioning validates Sage's binary-plus-auth direction. Use as product packaging/skill ergonomics reference, not as source parity truth. |
| runreal/unreal-mcp | https://github.com/runreal/unreal-mcp | Public GitHub repo; Python Remote Execution approach. | No dedicated Niagara tool list found in README/local scan; value is full Unreal Python API reach without custom plugin. | Not a Niagara benchmark. It is a "Python escape hatch" competitor, useful only for low-install-friction positioning. |
| remiphilippe/mcp-unreal | https://mcpservers.org/servers/remiphilippe/mcp-unreal | Listed as 49 tools with doc index and UE bridge. | Registry page lists `niagara_ops` with spawn_system, set/get parameter, activate/deactivate, list_emitters, set_emitter_enabled, get_system_info. | Broad but shallow Niagara coverage. Good reminder that docs/API lookup paired with editor tools matters. |
| UECortex | https://mcpmarket.com/server/uecortex | Marketplace/listing signal, not source-audited here. | Listing claims native UE5 plugin, embedded MCP server, 139 pure C++ tools across 13 categories including Niagara. | Treat as positioning reference until source/tool reference is available. |
| Codeturion unreal-api-mcp | https://github.com/Codeturion/unreal-api-mcp and https://pypi.org/project/unreal-api-mcp/ | PolyForm Noncommercial per package badges/listings; source-audited locally. | Not an editor mutation tool. It indexes UE C++ API docs including built-in plugins such as Niagara. | Strong companion idea: API signature lookup prevents hallucinated includes/deprecated calls. Sage's `read_engine_header`/`find_engine_symbol` should be positioned as integrated API-grounding. |

## Benchmark Delta Matrix

| Capability | Monolith | UE-MCP | ChiR24 | Sage after 2026-06-04 source work | Delta |
|---|---:|---:|---:|---:|---|
| Basic system/emitter/list/spawn/parameter | Strong | Strong | Strong | Strong | Covered, runtime dogfood pending. |
| Renderer CRUD/property writes | Strong incl. bindings/material helpers | Strong | Preset renderer adders | Generic CRUD/property writes | Add renderer binding/material convenience read/write. |
| Module stack read/write | Strong ordered stack with usage selectors | Basic graph/module pin writes | Preset module adders | Basic graph node list/add/remove/replace and pin default writes | Add real ordered usage contexts, emitter/system/stage/event selectors, override-map awareness. |
| Dynamic inputs | Full lifecycle and search | Not explicit | Not explicit | Read-only linked-pin dynamic-input candidate listing; assignment remains hard unsupported | Partial. Write-side requires exported safe node creation/stack API. |
| Custom HLSL | Direct existing `CustomHlsl` read/write plus module/function creation | HLSL module/scratch creation | Not primary | Existing CustomHlsl node text read/write implemented; custom-expression node creation and HLSL module/scratch creation remain hard unsupported | Partial. Creation-side remains high-priority but private/editor API bound. |
| Event handlers and simulation stages | Add/list/remove plus selectors | Not explicit in public niagara category | Event generator/receiver and sim-stage add | List/remove implemented; add is hard unsupported | Gap. |
| Search/discovery over Niagara assets | Parameter, DI, material, structural query, similarity, references | Asset list only | Not clear | Parameter/DI/material search, structural query, similarity, references, and system DI listing implemented | Covered for read-only discovery. |
| Data interfaces | Configure DI, DI functions, system DI listing | Inspect user-scope DIs | DI adders for mesh/spline/audio/collision | Cached DI inspection only | Gap for add/configure and DI function listing. |
| Temporal control/stateless emitters | Timing composite writers, stateless-emitter factory | Not explicit | GPU sim enable/stage | Warmup, fixed bounds, SimCache capture/read/compare | Partial. |
| Preview/capture | Niagara asset preview/GIF via editor preview tooling | Not primary | Debug shapes/preview implied | Preview spawn and SimCache; image capture hard unsupported | Gap for image/GIF capture. |
| Response/tool ergonomics | Namespace dispatch, discover/guide, response shaping, did_you_mean, cursor pagination | Category tool with action dispatch | Category/preset tool style | Explicit `niagara.*` tools with structured errors | Consider optional response shaping and alias/fuzzy error help. |

## Implementation Guidance For Sage

- Advertised Niagara tools must not return successful placeholder notes. Unsupported editor-private areas should return hard MCP errors with a precise reason.
- Public/editor-stable UE APIs should be preferred over non-exported Niagara stack helpers. Where only non-exported editor APIs exist, expose read-only diagnostics or explicit unsupported boundaries.
- Preserve Sage's differentiator: dependency/referrer diagnostics, shared-emitter mutation guards, dry-run/readback, and compile/save honesty should be stronger than broad action counts.
- Keep license boundaries clean. Public permissive repos may inform feature coverage and taxonomy, but Sage implementation should stay source-original and UE-API-backed.

## Current Sage Coverage After Source Implementation

- Implemented real read/write or readback-backed behavior for listing, preview spawn/cleanup, component parameters, system/emitter creation, emitter handles, module graph read/export/basic mutation, renderer CRUD, user parameters, collections, bounds/warmup/effect type/scalability, validation, sim cache capture/metadata, event/stage listing/removal, dependency/referrer audits, static switches, existing CustomHlsl text read/write, read-only dynamic-input candidate listing, Niagara search/discovery, and batch execution.
- Kept explicit unsupported hard-error boundaries for deterministic preview image capture, safe module move/duplicate, dynamic input assignment, custom expression node creation, HLSL module creation, scratch module creation, runtime collection value mutation, event-handler creation, and simulation-stage creation.
- New post-reference high-priority deltas: HLSL module/function authoring; ordered stack usage selectors; dynamic-input write lifecycle; event/sim-stage creation; DI configure/add helpers; renderer binding/material convenience; preview image/GIF capture.
- Runtime dogfood still needs a duplicated Kale Niagara asset and an editor session after build/package/deploy; this note only documents source-level implementation and external reference context.
