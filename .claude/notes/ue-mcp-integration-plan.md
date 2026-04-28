# UE-MCP → Sage Integration Roadmap

> **Source:** `/Users/mahmutalemdar/Developer/alemdarlabs/ue-mcp` (TypeScript MCP server + C++ plugin, 448 distinct actions, BUSL-1.1 → Apache after 4 yr).
> **Target:** Sage Unreal MCP — **443 tool handler · Phase 4 COMPLETE (2026-04-28)**
> **Parity:** 445 / 448 (%99.3) — 3 N/A (feedback.submit, demo.step, demo.cleanup)
> **Strategy (doğrulandı):** Full feature parity per subsystem. Read+write birlikte shipped. Scope cut yapılmadı.

> **NOT:** Bu belge entegrasyon planını kayıt altına alır. Plan tamamlanmıştır.
> Güncel coverage detayı için bkz. [`ue-mcp-tasks.md`](ue-mcp-tasks.md).

---

## License & Architecture Posture

ue-mcp is **BUSL-1.1** — non-compete clause prevents cloning the project's identity but does not block re-implementing capabilities (which is what we're doing). We don't fork code — we read it as a capability spec, then build native sage-bridge handlers in our own architecture (C++23 server, no TypeScript stdio).

ue-mcp's **architectural choices we will NOT adopt:**
- TypeScript stdio MCP server (we're C++23 HTTP+SSE — better for multi-editor, durable sessions)
- 562 actions in 19 mega-tools (we expose distinct MCP tools — easier for the agent to discover via `tools/list`)
- 30s sync RPC timeout per request (we already have async event channel for streamed deltas)
- Flow YAML executor (premature; LLM agents already orchestrate)

ue-mcp's **architectural choices we WILL adopt:**
- HandlerRegistry pattern — already in `FSageToolDispatch`
- GameThreadExecutor — already in `RunOnGameThread<>`
- JSON ↔ UProperty reflection covering TArray/TMap/TObjectPtr/TSubclassOf — partial, completing in 4.0
- SQLite FTS5 over assets — landing alongside the SQLite slot store

---

## Subsystem Plan — every subsystem ships whole

Each subsystem below is one Phase 4 milestone. Every milestone delivers full read+write. Order is by infrastructure dependency, not effort estimate.

### 4.0 — UProperty collections + asset reference plumbing

Foundation for everything below. Without this, half the tools fail on
`TArray<FVector>`, `TMap<FName, ...>`, `TObjectPtr<UStaticMesh>` properties.

- TArray ↔ JSON array round-trip (set + get + per-index modify)
- TMap ↔ JSON object round-trip (set + get + per-key modify + remove)
- TSet ↔ JSON array round-trip (with insertion-order semantics)
- TObjectPtr / FSoftObjectPtr asset reference resolution: accept
  `/Game/...` paths, write proper hard-or-soft pointers, surface
  unresolved as `null` with diagnostic
- TSubclassOf class-path resolution: resolve `/Script/Engine.Pawn`
  *or* `/Game/.../BP_Foo.BP_Foo_C`, validate the class is a subclass
  of the property's `MetaClass`
- FInstancedStruct round-trip
- FGameplayTag / FGameplayTagContainer round-trip with tag tree validation
- Per-element edit ops: `array_append`, `array_insert`, `array_remove_at`,
  `array_remove_value`, `map_set`, `map_remove`, `set_add`, `set_remove`
  for the existing modify_*_property family

**Lands as:** `SageToolHelpers` extension + new `set_property_array_op`
tool family. Updates every existing modify_*_property tool to accept
collection paths (`Components[2].StaticMesh`, `Tags{movement.run}`).

---

### 4.1 — Reflection (full UClass / UStruct / UEnum surface)

- `reflect_class(class_path)` → name, parent, modules, flags, all
  properties (with type/category/access/replication info), all functions
  (signature/access/pure flag), interfaces, child classes,
  CLASS_Native/CLASS_Abstract/CLASS_Interface flags
- `reflect_struct(struct_path)` → all fields with full type info
- `reflect_enum(enum_path)` → all values with display names + tooltips
- `list_classes(filter?, base_class?, include_native, include_blueprint)`
- `list_structs(filter?)`
- `list_enums(filter?)`
- `find_subclasses(class_path, max_depth?)` — extended `class_hierarchy`
- `find_implementers(interface_path)` — classes implementing UInterface

---

### 4.2 — Blueprint (read + write, full)

Blueprint is the largest UE authoring surface; full coverage is non-
negotiable for an "AI manages projects" agent.

#### Read
- `bp.read(path)` → parent class, components, var count, fn count,
  interfaces, generated class path, parent BP if any
- `bp.list_variables(path)` — name, type, default, access, category,
  tooltip, replication, instance-editable
- `bp.list_functions(path)` — signature, access, pure flag, blueprint-
  callable, network role, latent
- `bp.list_local_variables(path, fn_name)`
- `bp.read_function_graph(path, fn_name)` — node list + connections
  + position metadata
- `bp.read_event_graph(path)` — same for the BP's event graph
- `bp.read_construction_script(path)` — same for the construction script
- `bp.get_execution_flow(path, fn_name)` — BFS exec-pin traversal
- `bp.read_components(path)` — full SCS tree including default props
  on each component
- `bp.list_node_types(filter?)` — palette query
- `bp.search_nodes(path, kw)` — substring across all graphs
- `bp.get_dependencies(path)` — all referenced classes/functions/macros
- `bp.get_cdo_properties(path)` — class-default-object property snapshot

#### Write — graph editing
- `bp.add_node(path, fn_name, node_class, position)`
- `bp.delete_node(path, fn_name, node_id)`
- `bp.connect_pins(path, fn_name, src_node, src_pin, dst_node, dst_pin)`
- `bp.disconnect_pins(path, fn_name, ...)`
- `bp.set_node_property(path, fn_name, node_id, prop, value)`
- `bp.move_node(path, fn_name, node_id, new_pos)`
- `bp.export_nodes_t3d(path, fn_name, node_ids?)` — bulk copy
- `bp.import_nodes_t3d(path, fn_name, t3d, position?)` — bulk paste

#### Write — variable editing
- `bp.add_variable(path, name, type, default?, category?, access?, replication?)`
- `bp.delete_variable(path, name)`
- `bp.rename_variable(path, old, new)` — with reference fixup
- `bp.set_variable_default(path, name, value)`
- `bp.set_variable_properties(path, name, {access, category, replication, ...})`
- `bp.add_local_variable(path, fn_name, name, type, default?)`
- `bp.delete_local_variable(path, fn_name, name)`

#### Write — function editing
- `bp.add_function(path, name, signature)`
- `bp.delete_function(path, name)`
- `bp.rename_function(path, old, new)` — with caller fixup
- `bp.set_function_properties(path, name, {access, pure, callable, network})`
- `bp.add_function_input(path, fn_name, param_name, type, default?)`
- `bp.add_function_output(path, fn_name, param_name, type)`
- `bp.remove_function_param(path, fn_name, param_name)`

#### Write — class shape
- `bp.reparent(path, new_parent_class)` — change parent class with
  member/function reconciliation
- `bp.set_cdo_property(path, prop, value)` — write class default object
- `bp.add_interface(path, interface_class)`
- `bp.remove_interface(path, interface_class)`

#### Write — components
- `bp.add_bp_component(path, component_class, name, parent_socket?)` —
  add to SCS (different from runtime `add_component`)
- `bp.remove_bp_component(path, name)`
- `bp.reparent_component(path, name, new_parent_socket)`
- `bp.set_bp_component_property(path, name, prop, value)`

#### Compile + diagnostics
- `bp.compile(path)` — trigger full compile, return errors+warnings
- `bp.validate(path)` — compile-without-output dry run
- `bp.run_construction_script(path, location?, rotation?)` — spawn temp
  actor, return generated component snapshot, destroy

**Tally:** ~50 tools. Implements all of ue-mcp's blueprint category (49).

---

### 4.3 — Material (read + write + graph authoring + preview)

Sage has 1 material tool (`modify_material_parameter`). ue-mcp has 36.

#### Read
- `mat.read(path)` — domain, blend mode, shading model, parameter list,
  expression count, instruction count
- `mat.list_parameters(path)` — scalar/vector/texture/static-switch
- `mat.list_expressions(path)` — full graph node list
- `mat.read_graph(path)` — nodes + connections + positions
- `mat.get_shader_stats(path)` — texture samples, instructions, registers
- `mat.export_graph(path)` — full graph as JSON spec

#### Write — instances
- `mat.create(path, domain, blend_mode, shading_model)`
- `mat.create_instance(parent, dest)` — UMaterialInstanceConstant
- `mat.set_parameter(path, name, value)` — already covered by
  `modify_material_parameter`; alias under mat.* namespace
- `mat.set_static_switch(path, name, value)`

#### Write — graph authoring
- `mat.add_expression(path, expr_class, position)` — add a graph node
- `mat.delete_expression(path, node_id)`
- `mat.connect_expressions(path, src_node, src_output, dst_node, dst_input)`
- `mat.disconnect(path, dst_node, dst_input)`
- `mat.connect_to_property(path, src_node, src_output, mat_prop)` —
  base color / metallic / roughness / emissive / normal / opacity
- `mat.set_expression_value(path, node_id, value)` — edit a constant
- `mat.connect_texture(path, expr_id, texture_path)`
- `mat.set_shading_model(path, model)`
- `mat.set_base_color(path, color)`
- `mat.import_graph(path, json_spec)` — JSON spec → graph
- `mat.build_graph(path, declarative_spec)` — high-level builder

#### Preview + validate
- `mat.render_preview(path, size?)` — bake base color preview PNG
- `mat.validate(path)` — compile shader, return stats + warnings

---

### 4.4 — Index perf via Kuzu COPY FROM JSON

Current ingest: 8K asset / 16K dep / 8K class ≈ 30s. Target <5s.

- ingestSnapshot writes per-table NDJSON files to a temp dir
- `COPY <Table> FROM '<file>' (file_format='json')` per table
- Edge tables COPY via two-column FROM/TO format
- Cleanup tempdir on success
- Falls back to current batched-CREATE if COPY rejected (older Kuzu)
- Smoke harness measures 3-table 8K ingest end-to-end, asserts <5s

---

### 4.5 — Asset advanced (full ue-mcp asset surface)

ue-mcp has 49 asset actions. Sage has 8. Closing the gap.

- `get_mesh_bounds(path)` — bbox + extent (static + skeletal)
- `get_mesh_collision(path)` — collision primitive count + names + simple/complex
- `get_mesh_lod_info(path)` — LOD count, screen sizes, vertex counts
- `set_mesh_nav(path, settings)` — nav-mesh-relevant flags
- `set_sk_material_slots(path, slots)` — skeletal mesh material assignment
- `bulk_rename_assets([{src, dst}])` — single transaction multi-rename
- `move_folder(src, dst)` — folder move with redirector fixup
- `import_fbx(file, dest, options)`
- `import_texture(file, dest, options)`
- `import_audio(file, dest, options)`
- `create_datatable(dest, row_struct)`
- `read_datatable(path)`
- `set_datatable_row(path, row_name, values)`
- `delete_datatable_row(path, row_name)`
- `list_redirectors()`
- `fixup_redirectors(paths)`
- `diagnose_registry()` — disk vs memory state comparison
- `search_assets_fts(query, top_n?)` — SQLite FTS5 over names/classes/paths
- `reindex_fts()` — full FTS rebuild
- `get_asset_thumbnail(path)` — base64 PNG
- `get_asset_socket(mesh_path, socket_name)` — read socket transform
- `add_asset_socket(mesh_path, socket_name, transform)`
- `remove_asset_socket(mesh_path, socket_name)`

---

### 4.6 — Editor automation (full)

ue-mcp has 65 editor actions. Sage has 6.

- `console_command(cmd, allow_unsafe?)` — explicit-confirm gated
- `run_python(code)` — embedded Python execution (if PythonScriptPlugin loaded)
- `editor.list_dialogs()` / `respond_to_dialog(id, choice)` /
  `set_dialog_policy(policy)` — auto-respond modal dialogs (critical
  for headless ops)
- `take_screenshot(path?, viewport?)`
- `set_viewport_camera(loc, rot)`
- `set_viewport_mode(perspective|top|front|side|game)`
- `set_log_filter({category, level})`
- `read_log(category?, level?, since_ms?)` — recent log lines
- `get_engine_version()` / `get_project_version()` / `get_plugin_versions()`
- `play_in_editor_apply_damage(actor, amount, type)` — runtime PIE damage
- `editor.inspect_pie()` — current PIE world state
- `editor.get_pie_anim_state(actor)` — animation runtime state
- `editor.get_pie_subsystem_state(subsystem_class)` — query a
  GameInstanceSubsystem at runtime
- `play_sequencer(path)` / `stop_sequencer(path)`
- `read_sequencer_tracks(path)`
- `add_sequencer_track(path, track_class, binding)`

---

### 4.7 — Project / engine introspection

- `read_cpp_header(path)` — read project module header file
- `read_module(name)` — Build.cs structure + dependencies
- `list_modules()` — all modules in current project
- `search_cpp(pattern, scope?)` — grep across project source
- `read_engine_header(path)` — engine source under `Engine/Source/Runtime`
- `find_engine_symbol(name)` — symbol grep in engine source
- `read_config(ini_path)` / `search_config(pattern)` /
  `list_config_tags()` — INI tree introspection
- `get_project_settings()` — all DefaultGame.ini / DefaultEngine.ini
  settings as structured JSON
- `set_project_setting(key, value)` — write back through Project Settings
- `build()` — invoke UBT against the project
- `generate_project_files()` — UBT GenerateProjectFiles
- `get_build_targets()` / `get_engine_modules()` / `list_plugins()`
- `enable_plugin(name)` / `disable_plugin(name)`

---

### 4.8 — Animation (full)

56 actions in ue-mcp.

- AnimBlueprint: `read`, `create`, `read_anim_graph`, `add_state`,
  `add_transition`, `set_state_machine`, `compile`
- Montage: `create`, `read`, `set_sequence`, `add_section`,
  `set_montage_properties`
- Sequence: `create`, `read`, `set_bone_keyframes`, `get_bone_transforms`,
  `add_curve`, `set_root_motion`
- Blendspace: `create`, `read`, `add_sample`, `remove_sample`
- Composite: `create`, `read`, `add_segment`
- Skeleton: `get_info`, `list_sockets`, `add_virtual_bone`,
  `remove_virtual_bone`
- IK Rig: `create`, `read`, `add_solver`, `set_solver_setting`,
  `add_retarget_chain`
- ControlRig: `read`, `list_variables`, `set_variable`
- Modifiers: `list`, `add_modifier`, `remove_modifier`, `apply_modifier`

---

### 4.9 — Niagara VFX (full)

37 actions.

- System: `create`, `read`, `compile`, `add_emitter`, `remove_emitter`
- Emitter: `read`, `set_property`, `add_module`, `remove_module`
- Module: `list_inputs`, `set_input`, `list_static_switches`,
  `set_static_switch`, `create_from_hlsl`, `create_scratch`,
  `get_compiled_hlsl`
- Renderer: `list`, `add`, `remove`, `set_property`
- System spec: `create_system_from_spec` — declarative VFX builder
- Batch: `batch([{action, args}])` — fail-fast sequenced ops

---

### 4.10 — AI / Gameplay framework (full)

59 actions in ue-mcp's gameplay category.

- Physics: `set_collision_profile`, `set_simulate_physics`,
  `set_collision_enabled`, `set_physics_properties`
- Navigation: `rebuild_navigation`, `get_navmesh_info`, `project_to_nav`,
  `spawn_nav_modifier`, `get_navmesh_details`
- Enhanced Input: `create_input_action`, `create_input_mapping`,
  `read_imc`, `add_imc_mapping`, `set_mapping_modifiers`,
  `remove_imc_mapping`, `set_imc_mapping_key`, `set_imc_mapping_action`
- Behavior Trees: `list_bts`, `get_bt_info`, `read_bt_graph`,
  `create_bt`, `add_bt_node`, `connect_bt_nodes`
- EQS: `create_eqs_query`, `read_eqs_query`, `add_eqs_test`
- StateTree: `create_state_tree`, `read_state_tree`, `add_state`,
  `add_transition`
- SmartObject: `create_smart_object_def`, `read_smart_object_def`,
  `add_slot`, `add_definition_data`
- Perception: `add_perception`, `configure_sense`, `read_perception`
- Game framework: `create_game_mode`, `create_game_state`,
  `create_player_controller`, `create_player_state`, `create_hud`,
  `set_world_game_mode`, `get_framework_info`
- Runtime PIE inspection: `inspect_pie`, `get_pie_anim_state`,
  `get_pie_anim_properties`, `get_pie_subsystem_state`,
  `apply_damage_in_pie`

---

### 4.11 — UMG / Widget (full)

21 actions.

- Widget: `read`, `create`, `read_tree`, `add_widget`, `remove_widget`,
  `set_widget_property`, `set_slot_property`
- Animation: `create_animation`, `read_animations`, `add_track`
- EUW: `create_utility_widget`, `run_utility_widget`
- EUB: `create_utility_blueprint`, `run_utility_blueprint`
- Layout: `set_anchors`, `set_alignment`, `set_padding`

---

### 4.12 — PCG (Procedural Content Generation)

23 actions.

- Graph: `create`, `read`, `add_node`, `connect_nodes`,
  `set_node_settings`, `remove_node`
- Mesh spawner: `set_static_mesh_spawner_meshes`, `read_spawner`
- Component query: `query_components`, `set_component_property`
- Volume: `place_volume`, `set_volume_bounds`

---

### 4.13 — Landscape (full)

14 actions.

- Sculpt: `sculpt(operation, brush, position, strength)`
- Paint: `paint_layer(layer, brush, position, strength)`
- Splines: `read_splines`, `add_spline`, `add_spline_segment`,
  `set_spline_property`
- Heightmap: `import_heightmap(file, settings)`,
  `export_heightmap(path)`
- Material: `set_landscape_material(path)`

---

### 4.14 — Foliage (full)

13 actions.

- Type: `create_type`, `read_type`, `set_type_settings`, `delete_type`
- Painting: `paint(brush, position, strength)`,
  `erase(brush, position, strength)`
- Instances: `sample(area)`, `select(filter)`, `remove_selected`,
  `transform_selected`

---

### 4.15 — GAS (Gameplay Ability System)

18 actions.

- Attributes: `create_attribute_set`, `read_attribute_set`,
  `add_attribute`
- Abilities: `create_ability`, `read_ability`, `set_ability_property`
- Effects: `create_effect`, `read_effect`, `set_effect_modifier`,
  `set_effect_executions`
- Cues: `create_cue`, `read_cue`, `set_cue_response`

---

### 4.16 — Networking

14 actions.

- Replication: `set_replicated_property`, `list_replicated_properties`,
  `set_replication_condition`
- Network: `set_dormancy`, `set_relevancy`, `set_net_priority`,
  `set_cull_distance`, `set_replicates`, `set_replicate_movement`

---

### 4.17 — Audio

9 actions.

- Sound: `create_sound_cue`, `create_metasound`, `read_sound_cue`
- Playback: `play_at_location`, `play_attached`, `stop_all`
- Ambient: `spawn_ambient_actor`, `set_ambient_property`

---

### 4.18 — Source control extras

Beyond Sage's `get_source_control_state` + `checkout_files`:

- `revert_files(paths)`
- `submit(paths, message)`
- `add_files(paths)`
- `delete_files(paths)`
- `get_file_state(path)` — synced/locked/modified
- `get_history(path)` — change log

---

### 4.19 — Reporting / observability

- `report_issue(title, body, labels?)` — write to
  `.claude/notes/lessons.md` (or, if configured, GitHub via gh CLI)
- `get_session_log(since_ms?, level?)` — recent log slice from
  `/tmp/sage-server.log`
- `get_metrics()` — pending RPC count, slot count, cache hit/miss,
  ingest avg ms, query avg ms

---

### 4.20 — Headless test mode

- Plugin's filesystem-only handlers (INI parsing, header reading,
  asset listing via on-disk pak/uasset inspection) usable without
  a running editor world
- Mock plugin `sage-bridge-mock-plugin` extended with handler set
  for read-only MCP tools, so CI can exercise Phase 4.0–4.7 without
  booting UE
- Catch2 unit suite expanded across new plugin handlers

---

## Won't ship

The only items genuinely off-roadmap. None of these are scope cuts —
they're either footguns or duplicates.

| Item | Reason |
|---|---|
| ue-mcp `flow` (YAML executor) | The LLM is the orchestrator. Adding YAML inserts an intermediate language without payoff. |
| ue-mcp `feedback` auto-GitHub-issue | We have `report_issue` writing to lessons.md; gh CLI integration optional via env var. |
| ue-mcp `demo.build_neon_shrine` | Project-specific marketing demo. |

---

## Implementation order

The graph of work matters; the calendar doesn't.

```
4.0 UProperty collections + asset refs
 │
 ├──► 4.1 Reflection (depends on 4.0 for property dump)
 ├──► 4.2 Blueprint (read + write, depends on 4.0 + 4.1)
 ├──► 4.3 Material (depends on 4.0)
 ├──► 4.5 Asset advanced (depends on 4.0)
 ├──► 4.6 Editor automation (mostly independent)
 ├──► 4.7 Project / engine introspection (mostly independent)
 ├──► 4.8 Animation (depends on 4.0)
 ├──► 4.9 Niagara (depends on 4.0)
 ├──► 4.10 AI / Gameplay (depends on 4.0)
 ├──► 4.11 UMG (depends on 4.0)
 ├──► 4.12 PCG (depends on 4.0)
 ├──► 4.13 Landscape (independent)
 ├──► 4.14 Foliage (independent)
 ├──► 4.15 GAS (depends on 4.0 + 4.10 for tag containers)
 ├──► 4.16 Networking (depends on 4.1 reflection)
 ├──► 4.17 Audio (depends on 4.0)
 ├──► 4.18 Source control extras (independent)
 ├──► 4.19 Reporting (independent)
 └──► 4.20 Headless test mode (transversal — extends as new tools land)

4.4 Index perf — transversal, lands when current ingest gets unwieldy.
```

Each milestone ships its full subsystem. Tools land as commits within
their milestone. New `SageXxxTools.cpp` plugin handler groups per major
subsystem (Blueprint, Material, Niagara, Animation, Landscape, etc.)
mirror the structure already used for Actor/Asset/Editor/Material.
