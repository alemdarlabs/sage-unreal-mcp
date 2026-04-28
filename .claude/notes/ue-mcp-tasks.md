# UE-MCP → Sage: 448 Action Per-Tool Task List

> **Source:** `/Users/mahmutalemdar/Developer/alemdarlabs/ue-mcp` — TypeScript MCP server + C++ plugin, BUSL-1.1.
> **Audit date:** 2026-04-28 (last update: Phase 4.2-r2g part 2 T3D clipboard shipped).
> **Status:** Sage 130 tools · UE-MCP 448 actions · ~118 covered (mostly via Phase 4) · **330 actions remain**.
>
> Earlier note had cited 562; actual enumeration of every `RegisterHandler` / dispatcher branch in the ue-mcp source landed on 448. The 562 number likely came from including duplicates / aliases / TS-side validation rules that don't materialise as distinct C++ handlers.

## Coverage summary

| Category | Have | Need | Total | % |
|---|---:|---:|---:|---:|
| project | 15 | 14 | 29 | 52 |
| editor | 25 | 19 | 44 | 57 |
| gameplay | 2 | 43 | 45 | 4 |
| animation | 0 | 46 | 46 | 0 |
| blueprint | 38 | 8 | 46 | 83 |
| asset | 7 | 32 | 39 | 18 |
| level | 10 | 22 | 32 | 31 |
| niagara | 0 | 26 | 26 | 0 |
| material | 11 | 16 | 27 | 41 |
| pcg | 0 | 16 | 16 | 0 |
| widget | 0 | 17 | 17 | 0 |
| gas | 0 | 9 | 9 | 0 |
| networking | 0 | 11 | 11 | 0 |
| landscape | 0 | 11 | 11 | 0 |
| foliage | 0 | 7 | 7 | 0 |
| reflection | 4 | 2 | 6 | 67 |
| audio | 0 | 5 | 5 | 0 |
| feedback | 0 | 1 | 1 | 0 |
| demo | 0 | 2 | 2 | 0 |
| **TOTAL** | **149** | **299** | **448** | **33** |

Highest leverage (raw action count missing): gameplay (+43), animation (+46), blueprint (+33), editor (+34), asset (+32), project (+27), niagara (+26), pcg (+16), material (+16), widget (+17).

---

## How to use this list

Each row is one `TODO` for the agent. Format:

`[STATUS]` `category.action` — purpose · type · Sage match (if any)

`[ ]` = TODO. `[~]` = partially covered (semantic overlap only — Sage has a related tool but with different scope/signature; verify before declaring done). `[x]` = covered.

Group dependency notes:
- **Phase 4.0 collections groundwork** is required for any action that takes/returns TArray/TMap/TObjectPtr/TSubclassOf/UStruct.
- **GameThread marshalling** required for every mutation — see `feedback_gamethread_marshal` memory.
- **PIE rejection** required for any mutation that touches package/blueprint/asset state.

---

## project (29) — Sage covers 2

C++ source / header / config / build introspection. Largest pure-read surface in ue-mcp; valuable for agents that want to "understand the project" before mutating.

- [~] `project.get_status` — Server mode + editor connection · R · Sage `get_world` (partial, different scope)
- [ ] `project.set_project` — Switch project + reconnect · W
- [x] `project.get_info` → `project.get_info` (Phase 4.7-p1; .uproject parse, plugins, declared modules)
- [x] `project.read_config` → `project.read_config` (Phase 4.7-p3; sectioned parse + raw mode + UE INI +/-/!/. modifier capture)
- [x] `project.search_config` → `project.search_config` (Phase 4.7-p3; substring across all *.ini under Config/)
- [x] `project.list_config_tags` → `project.list_config_tags` (Phase 4.7-p3; +GameplayTagList=(Tag="...") pattern scan)
- [x] `project.read_cpp_header` → `project.read_cpp_header` (Phase 4.7-p1; heuristic regex UCLASS/USTRUCT/UENUM scan)
- [ ] `project.read_module` — Read module source with header/source counts · R
- [x] `project.list_modules` → `project.list_modules` (Phase 4.7-p1; Source/<X>/<X>.Build.cs scan with header/source counts)
- [x] `project.search_cpp` → `project.search_cpp` (Phase 4.7-p2; .h/.cpp/.inl substring scan with snippet)
- [x] `project.read_engine_header` → `project.read_engine_header` (Phase 4.7-p2; alias to read_cpp_header with EngineDir guard)
- [x] `project.find_engine_symbol` → `project.find_engine_symbol` (Phase 4.7-p2; Runtime/Editor/Developer/ThirdParty category-scoped grep)
- [x] `project.list_engine_modules` → `project.list_engine_modules` (Phase 4.7-p2; Engine/Source/{Runtime,Editor,Developer,ThirdParty} = 492 modules total in UE 5.7)
- [ ] `project.search_engine_cpp` — Search Runtime/Editor/Developer/Plugins · R
- [x] `project.set_config` → `project.set_config` (Phase 4.7-p4; backup-then-rename atomic write, +/-/!/. modifier support, can bootstrap missing files)
- [~] `project.build` — Build C++ · W · Sage `compile_and_reload` (Win-only LC alias)
- [ ] `project.generate_project_files` — UBT GenerateProjectFiles · W
- [ ] `project.create_cpp_class` — Create native UCLASS in module · W
- [ ] `project.list_project_modules` — List native modules with paths · R
- [ ] `project.live_coding_compile` — Trigger LC compile · W
- [ ] `project.live_coding_status` — LC availability · R · Sage has `get_live_coding_status` (Phase 1) — **mark as `[x]`** instead
- [ ] `project.write_cpp_file` — Write .h/.cpp/.inl under Source/ · W
- [x] `project.read_cpp_source` → `project.read_cpp_source` (Phase 4.7-p1; .h/.cpp/.inl read with max_bytes cap + path safety guard)
- [ ] `project.add_module_dependency` — Append to Build.cs deps · W

**Phase 4.7 candidates:** read_cpp_header, read_module, list_modules, search_cpp, read_engine_header, find_engine_symbol, read_config, search_config, list_config_tags, get_info, set_config, list_project_modules. ~12 read-side tools, mostly filesystem-only / no UE editor needed → **headless-friendly**.

---

## editor (44) — Sage covers 10

- [x] `editor.start_editor` — Launch editor · W · *implicit via shell*
- [x] `editor.stop_editor` — Close editor · W · *implicit*
- [x] `editor.restart_editor` — Stop + start · W · Sage `restart_editor`
- [x] `editor.execute_command` — Console command · W · Sage `editor.console_command`
- [ ] `editor.execute_python` — Run Python in editor · W
- [ ] `editor.run_python_file` — Run Python script · W
- [x] `editor.set_property` → `editor.set_property` (Phase 4.6-r3-b2; SetUPropertyFromJson via FProperty reflection, structured JSON value, PIE rejected)
- [x] `editor.play_in_editor` — PIE control · W · Sage `run_pie`/`stop_pie`
- [~] `editor.get_runtime_value` — Read PIE actor property · R · Sage `get_pie_state` (partial)
- [x] `editor.set_pie_time_scale` → `editor.set_pie_time_scale` (Phase 4.6-r3-b2; AWorldSettings caps lifted + SetGlobalTimeDilation, requires active PIE)
- [ ] `editor.hot_reload` — Hot reload C++ · W
- [x] `editor.undo` → `editor.undo` (Phase 4.6-r3-b1; GEditor->UndoTransaction(true))
- [x] `editor.redo` → `editor.redo` (Phase 4.6-r3-b1; GEditor->RedoTransaction)
- [ ] `editor.get_perf_stats` — Editor performance stats · R
- [ ] `editor.run_stat` — STAT command for profiling · W (covered indirectly by `console_command`)
- [ ] `editor.set_scalability` — Quality/scalability level · W
- [x] `editor.capture_screenshot` — Take screenshot · W · Sage `editor.take_screenshot`
- [ ] `editor.capture_scene_png` — Headless PNG via SceneCapture2D · W
- [x] `editor.get_viewport` — Viewport camera state · R · Sage `get_viewport_state`
- [x] `editor.set_viewport` → `editor.set_viewport` (Phase 4.6-r3-b1; FLevelEditorViewportClient SetViewLocation/Rotation, both optional)
- [x] `editor.focus_on_actor` → `editor.focus_on_actor` (Phase 4.6-r3-b1; GEditor->MoveViewportCamerasToActor)
- [ ] `editor.create_sequence` — Create LevelSequence · W
- [ ] `editor.get_sequence_info` — Read sequence · R
- [ ] `editor.add_sequence_track` — Add track to LS · W
- [ ] `editor.play_sequence` — Play/stop/pause LS · W
- [ ] `editor.build_all` — Build geometry+lighting+paths+HLOD · W
- [ ] `editor.build_geometry` — Rebuild BSP · W
- [ ] `editor.build_hlod` — Build HLODs · W
- [ ] `editor.validate_assets` — Run validation on directory · W
- [ ] `editor.get_build_status` — Build/map compile status · R
- [ ] `editor.cook_content` — Cook for platform · W
- [x] `editor.get_log` — Read log with filter · R · Sage `editor.read_log`
- [x] `editor.search_log` → `editor.search_log` (Phase 4.6-r3-b3; substring scan with line + 500-char snippet)
- [ ] `editor.get_message_log` — Read message log · R
- [x] `editor.list_crashes` → `editor.list_crashes` (Phase 4.6-r3-b3; ~/Library/.../UnrealEngine/Saved/Crashes scan, mtime-sorted)
- [x] `editor.get_crash_info` → `editor.get_crash_info` (Phase 4.6-r3-b3; CrashContext.runtime-xml ErrorMessage+CallStack extract + 50-line log tail)
- [x] `editor.check_for_crashes` → `editor.check_for_crashes` (Phase 4.6-r3-b3; within_hours threshold)
- [x] `editor.set_dialog_policy` → `editor.set_dialog_policy` (Phase 4.6-r2)
- [x] `editor.clear_dialog_policy` → `editor.clear_dialog_policy` (Phase 4.6-r2)
- [x] `editor.get_dialog_policy` → `editor.get_dialog_policy` (Phase 4.6-r2)
- [x] `editor.list_dialogs` → `editor.list_dialogs` (Phase 4.6-r2)
- [x] `editor.respond_to_dialog` → `editor.respond_to_dialog` (Phase 4.6-r2)
- [ ] `editor.open_asset` — Open asset in its editor · W
- [ ] `editor.reload_bridge` — Hot-reload Python bridge · W · *not applicable (we're C++)*

**Phase 4.6 round 2 — DONE (5 tools, 4.6-r2 commit):** dialog_policy quartet (set/clear/get/list_dialogs/respond_to_dialog) — `FCoreDelegates::ModalMessageDialog` hook + Slate widget tree traversal + button click simulation. Lazy install on first set_dialog_policy. Conservative default-response (no/cancel) when no policy matches.

**Phase 4.6 round 3 candidates:** set_property, undo/redo, focus_on_actor, set_viewport, sequence_*, build_*, set_pie_time_scale, validate_assets, get_perf_stats, run_python.

---

## gameplay (45) — Sage covers 2

- [ ] `gameplay.set_collision_profile` — Collision preset on actor · W
- [ ] `gameplay.set_simulate_physics` — Toggle physics · W
- [ ] `gameplay.set_collision_enabled` — Collision mode · W
- [ ] `gameplay.set_physics_properties` — Mass/damping/gravity · W
- [ ] `gameplay.rebuild_navigation` — Rebuild navmesh · W
- [ ] `gameplay.get_navmesh_info` — Nav system + settings · R
- [ ] `gameplay.project_to_nav` — Project point · R
- [ ] `gameplay.spawn_nav_modifier` — Place nav modifier · W
- [ ] `gameplay.create_input_action` — Create IA asset · W
- [ ] `gameplay.create_input_mapping` — Create IMC asset · W
- [ ] `gameplay.list_input_assets` — List input assets · R
- [ ] `gameplay.read_imc` — Read IMC mappings · R
- [ ] `gameplay.list_input_mappings` — Key→action bindings · R
- [ ] `gameplay.add_imc_mapping` — Add key mapping · W
- [ ] `gameplay.set_mapping_modifiers` — Modifiers/triggers · W
- [ ] `gameplay.remove_imc_mapping` — Remove by index/key · W
- [ ] `gameplay.set_imc_mapping_key` — Rebind to new key · W
- [ ] `gameplay.set_imc_mapping_action` — Retarget IA · W
- [ ] `gameplay.list_behavior_trees` — List BTs · R
- [ ] `gameplay.get_behavior_tree_info` — BT + blackboard · R
- [ ] `gameplay.read_behavior_tree_graph` — Walk BT tree · R
- [ ] `gameplay.create_blackboard` — Create BB asset · W
- [ ] `gameplay.create_behavior_tree` — Create BT asset · W
- [ ] `gameplay.create_eqs_query` — Create EQS · W
- [ ] `gameplay.list_eqs_queries` — List EQS queries · R
- [ ] `gameplay.add_perception` — Add AIPerception to BP · W
- [ ] `gameplay.configure_sense` — Configure sense · W
- [ ] `gameplay.create_state_tree` — Create StateTree · W
- [ ] `gameplay.list_state_trees` — List STs · R
- [ ] `gameplay.add_state_tree_component` — Add STC to BP · W
- [ ] `gameplay.create_smart_object_def` — Create SOD · W
- [ ] `gameplay.add_smart_object_component` — Add SOC to BP · W
- [~] `gameplay.inspect_pie` — PIE runtime state · R · Sage `get_pie_state` (partial)
- [ ] `gameplay.get_pie_anim_state` — PIE AnimInstance state · R
- [ ] `gameplay.get_pie_anim_properties` — UPROPERTYs on AnimInstance · R
- [ ] `gameplay.get_pie_subsystem_state` — UPROPERTYs on subsystem · R
- [ ] `gameplay.create_game_mode` — GameMode BP · W
- [ ] `gameplay.create_game_state` — GameState BP · W
- [ ] `gameplay.create_player_controller` — PC BP · W
- [ ] `gameplay.create_player_state` — PS BP · W
- [ ] `gameplay.create_hud` — HUD BP · W
- [ ] `gameplay.set_world_game_mode` — World GM override · W
- [ ] `gameplay.get_framework_info` — Level framework classes · R
- [ ] `gameplay.get_navmesh_details` — RecastNavMesh params · R
- [ ] `gameplay.apply_damage_in_pie` — Apply damage to PIE actor · W

**Phase 4.10 milestone** — split into sub-phases: input system (8), AI/BT/EQS/StateTree/SmartObject (15), physics/nav (8), framework BPs + PIE inspection (14).

---

## animation (46) — Sage covers 0

- [ ] `animation.read_anim_blueprint` · R
- [ ] `animation.read_montage` · R
- [ ] `animation.read_sequence` · R
- [ ] `animation.read_blendspace` · R
- [ ] `animation.list` — List animation assets · R
- [ ] `animation.create_montage` · W
- [ ] `animation.create_anim_blueprint` · W
- [ ] `animation.create_blendspace` · W
- [ ] `animation.add_notify` · W
- [ ] `animation.get_skeleton_info` · R
- [ ] `animation.list_sockets` · R
- [ ] `animation.list_skeletal_meshes` · R
- [ ] `animation.get_physics_asset` · R
- [ ] `animation.create_sequence` · W
- [ ] `animation.set_bone_keyframes` · W
- [ ] `animation.get_bone_transforms` · R
- [ ] `animation.set_montage_sequence` · W
- [ ] `animation.set_montage_properties` · W
- [ ] `animation.create_state_machine` · W
- [ ] `animation.add_state` · W
- [ ] `animation.add_transition` · W
- [ ] `animation.set_state_animation` · W
- [ ] `animation.set_transition_blend` · W
- [ ] `animation.read_state_machine` · R
- [ ] `animation.read_anim_graph` · R
- [ ] `animation.add_curve` · W
- [ ] `animation.set_montage_slot` · W
- [ ] `animation.add_montage_section` · W
- [ ] `animation.create_ik_rig` · W
- [ ] `animation.read_ik_rig` · R
- [ ] `animation.list_control_rig_variables` · R
- [ ] `animation.set_root_motion` · W
- [ ] `animation.add_virtual_bone` · W
- [ ] `animation.remove_virtual_bone` · W
- [ ] `animation.create_composite` · W
- [ ] `animation.list_modifiers` · R
- [ ] `animation.create_ik_retargeter` · W
- [ ] `animation.set_anim_blueprint_skeleton` · W
- [ ] `animation.read_bone_track` · R
- [ ] `animation.create_pose_search_database` · W
- [ ] `animation.set_pose_search_schema` · W
- [ ] `animation.add_pose_search_sequence` · W
- [ ] `animation.build_pose_search_index` · W
- [ ] `animation.read_pose_search_database` · R
- [ ] `animation.set_sequence_properties` · W
- [ ] `animation.bake_root_motion_from_bone` · W

**Phase 4.8 milestone** — needs new `SageAnimationTools.cpp` plugin handler group + Anim/IKRig/ControlRig/PoseSearch module deps in Build.cs.

---

## blueprint (46) — Sage covers 13 (Phase 4.2 round 1)

- [x] `blueprint.read` → `bp.read`
- [x] `blueprint.list_variables` → `bp.list_variables`
- [x] `blueprint.list_functions` → `bp.list_functions`
- [x] `blueprint.read_graph` → `bp.read_function_graph`
- [ ] `blueprint.read_graph_summary` — Lightweight ~10KB summary · R
- [x] `blueprint.get_execution_flow` → `bp.get_execution_flow`
- [x] `blueprint.get_dependencies` → `bp.get_dependencies` (Phase 4.2-r2g/p5; AssetRegistry forward/reverse + class refs)
- [x] `blueprint.create` → `bp.create` (Phase 4.2-r2f; UBlueprintFactory + IAssetTools::CreateAsset, idempotent, parent class short-name + full path)
- [x] `blueprint.add_variable` → `bp.add_variable`
- [x] `blueprint.set_variable_properties` → `bp.set_variable_properties` (Phase 4.2-r2g/p5; instance_editable, blueprint_readonly, replicated, transient, save_game, expose_on_spawn, category, tooltip; calls CompileBlueprint at end)
- [ ] `blueprint.create_function` — Create user fn · W
- [x] `blueprint.delete_function` → `bp.delete_function`
- [x] `blueprint.rename_function` → `bp.rename_function` (Phase 4.2-r2d)
- [x] `blueprint.add_node` → `bp.add_node` (Phase 4.2-r2a)
- [x] `blueprint.delete_node` → `bp.delete_node`
- [x] `blueprint.set_node_property` → `bp.set_node_property` (Phase 4.2-r2a)
- [x] `blueprint.connect_pins` → `bp.connect_pins`
- [x] `blueprint.add_component` → `add_component` (Phase 1, runtime path; SCS path = `add_bp_component` not yet)
- [x] `blueprint.remove_component` → `remove_component` (same caveat)
- [x] `blueprint.set_component_property` → `modify_component_property`
- [x] `blueprint.get_component_property` → `bp.get_component_property` (Phase 4.2-r2g/p3)
- [x] `blueprint.set_class_default` → `bp.set_cdo_property`
- [x] `blueprint.delete_variable` → `bp.delete_variable`
- [x] `blueprint.add_function_parameter` → `bp.add_function_parameter` (Phase 4.2-r2e; supports input + output; FunctionResult auto-spawn)
- [x] `blueprint.list_function_parameters` → `bp.list_function_parameters` (Phase 4.2-r2e, NEW vs ue-mcp)
- [x] `blueprint.remove_function_parameter` → `bp.remove_function_parameter` (Phase 4.2-r2e, NEW vs ue-mcp)
- [x] `blueprint.set_variable_default` → `bp.set_variable_default`
- [x] `blueprint.compile` → `bp.compile`
- [x] `blueprint.list_node_types` → `bp.list_node_types` (Phase 4.2-r2a; supports filter substring)
- [~] `blueprint.search_node_types` → `bp.list_node_types` with filter parameter (semantic match)
- [x] `blueprint.create_interface` → `bp.create_interface` (Phase 4.2-r2f; UBlueprintInterfaceFactory)
- [x] `blueprint.add_interface` → `bp.add_interface` (Phase 4.2-r2c)
- [x] `blueprint.list_interfaces` → `bp.list_interfaces` (Phase 4.2-r2c, NEW vs ue-mcp)
- [x] `blueprint.remove_interface` → `bp.remove_interface` (Phase 4.2-r2c, NEW vs ue-mcp)
- [x] `blueprint.list_graphs` → `bp.list_graphs` (Phase 4.2-r2d; ubergraph/function/delegate/macro)
- [x] `blueprint.add_event_dispatcher` → `bp.add_event_dispatcher` (Phase 4.2-r2g/p1; signature graph + member variable, payload-params deferred to r2h)
- [x] `blueprint.list_event_dispatchers` → `bp.list_event_dispatchers` (Phase 4.2-r2g/p1, NEW vs ue-mcp)
- [x] `blueprint.remove_event_dispatcher` → `bp.remove_event_dispatcher` (Phase 4.2-r2g/p1, NEW vs ue-mcp)
- [ ] `blueprint.duplicate` — Duplicate BP · W
- [x] `blueprint.add_local_variable` → `bp.add_local_variable` (Phase 4.2-r2b)
- [x] `blueprint.list_local_variables` → `bp.list_local_variables` (Phase 4.2-r2b)
- [x] `blueprint.validate` → `bp.validate` (Phase 4.2-r2g/p4; SkipSave + silent FCompilerResultsLog)
- [x] `blueprint.read_component_properties` → `bp.read_component_properties` (Phase 4.2-r2g/p3; 58 props on SpringArmComponent verified)
- [x] `blueprint.read_node_property` → `bp.read_node_property` (Phase 4.2-r2a)
- [x] `blueprint.reparent_component` → `bp.reparent_component` (Phase 4.2-r2g/p3; cycle guard + self guard)
- [x] `blueprint.reparent` → `bp.reparent`
- [ ] `blueprint.set_actor_tick_settings` — CDO tick settings · W
- [x] `blueprint.export_nodes_t3d` → `bp.export_nodes_t3d` (Phase 4.2-r2g/p2; FEdGraphUtilities::ExportNodesToText, optional node_ids filter)
- [x] `blueprint.import_nodes_t3d` → `bp.import_nodes_t3d` (Phase 4.2-r2g/p2; CanImportNodesFromText pre-flight, fresh GUIDs, optional pos_x/pos_y re-center)
- [x] `blueprint.set_cdo_property` → `bp.set_cdo_property`
- [x] `blueprint.get_cdo_properties` → `bp.get_cdo_properties` (Phase 4.2-r2g/p5; class lookup short-name + full path, optional property filter)
- [x] `blueprint.run_construction_script` → `bp.run_construction_script` (Phase 4.2-r2g/p4; transient SpawnActor + RerunConstructionScripts + DestroyActor; Actor-derived guard)

**Phase 4.2 round 2 progress:**
- **r2a DONE (4 tools):** add_node + set_node_property + read_node_property + list_node_types. 12+3 smoke tests green on UE 5.7 SageTest. Branch (K2Node_IfThenElse) + CallFunction (with function_name/target_class) + GetVar/SetVar (with variable_name) spawn paths verified. Pin default round-trip (true→false→read) confirmed via Schema::TrySetDefaultValue.
- **r2b DONE (3 tools):** list_local_variables + add_local_variable + delete_local_variable. 11/11 smoke green: empty list → add MyLocal:int=42 → add MyBool:bool=true → list 2 → duplicate add -32602 → delete 1 → delete missing removed=0 (idempotent) → list 1 → cleanup. Backed by FBlueprintEditorUtils::AddLocalVariable + direct UK2Node_FunctionEntry::LocalVariables removal (avoids UE 5.7 RemoveLocalVariable scope-param churn).
- **r2c DONE (3 tools):** list_interfaces + add_interface + remove_interface. FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath) + RemoveInterface. UInterface guard via IsChildOf(UInterface::StaticClass()). 7-step round-trip green: add NavMovementInterface (already=false) → re-add (already=true, idempotent) → list 2 → remove (removed=1) → list 1 → re-remove (removed=0, idempotent) → cleanup. Plus error guards: missing path -32602, non-existent class -32602, non-UInterface class -32602.
- **r2d DONE (2 tools):** list_graphs (ubergraph/function/delegate/macro enumeration with node count) + rename_function (FBlueprintEditorUtils::RenameGraph w/ collision + same-name + missing guards). 9/9 smoke green: baseline list shows template's 4 graphs (EventGraph + UCS + Move + Aim) → add OldFn → rename OldFn→NewFn → verify list reflects → guards: nonexistent/collision/same-name all -32602.
- **r2e DONE (3 tools):** list/add/remove_function_parameter. UK2Node_EditablePinBase::CreateUserDefinedPin + RemoveUserDefinedPinByName. direction='input' (default) targets FunctionEntry, 'output' targets FunctionResult (auto-spawned with matching FunctionReference). Pin direction: input → entry's EGPD_Output pin; output → result's EGPD_Input pin (UE convention). 12/12 smoke green: empty → add 2 in + 1 out (FunctionResult auto-spawn) → list (2/1) → duplicate -32602 → remove 1 in + 1 out → remove missing removed=0 → list (1/0) → cleanup.
- **r2f DONE (2 tools):** bp.create + bp.create_interface. UBlueprintFactory (parent class short-name + full /Script path resolution) + UBlueprintInterfaceFactory. Idempotent (already=true on existing path). 10/10 smoke green + integration: created Test_NewBP, created Test_NewIface, duplicated Test_NewBP→Test_NewBP_Iface, added Test_NewIface_C as interface on Test_NewBP_Iface, list_interfaces confirmed. Full BP authoring loop closed: agent can now create asset → add var/fn → add params → add nodes → connect pins → implement interfaces → compile, all from Sage MCP without UE editor manual interaction.
- **r2g remaining (~8 tools):** T3D import/export pair, BP component CRUD via SCS path, validate, run_construction_script, dispatcher (add_event_dispatcher + list/remove), set_variable_properties, read_component_properties, get_cdo_properties, get_dependencies, reparent_component.

---

## asset (39) — Sage covers 7

- [ ] `asset.list` — List in directory · R
- [ ] `asset.search` — Search by name/class/path · R
- [ ] `asset.read` — Read via reflection · R
- [ ] `asset.read_properties` — Property dump · R
- [x] `asset.duplicate` → `duplicate_asset`
- [x] `asset.rename` → `rename_asset`
- [x] `asset.bulk_rename` → `asset.bulk_rename`
- [x] `asset.move` → `move_asset`
- [x] `asset.delete` → `delete_asset`
- [ ] `asset.delete_batch` — Batch with status · W
- [ ] `asset.create_data_asset` — DataAsset instance · W
- [x] `asset.save` → `save_assets`
- [ ] `asset.set_mesh_material` — Material on slot · W
- [ ] `asset.recenter_pivot` — Mesh pivot to center · W
- [ ] `asset.import_static_mesh` — From FBX/OBJ · W
- [ ] `asset.import_skeletal_mesh` — From FBX · W
- [ ] `asset.import_animation` — From FBX · W
- [ ] `asset.import_texture` — From image · W
- [ ] `asset.reimport` — Reimport from source · W
- [ ] `asset.read_datatable` — Read rows · R
- [ ] `asset.create_datatable` — Create asset · W
- [ ] `asset.reimport_datatable` — From JSON · W
- [ ] `asset.list_textures` — List textures · R
- [ ] `asset.get_texture_info` — Settings · R
- [ ] `asset.set_texture_settings` — Compression/LOD/sRGB · W
- [ ] `asset.add_socket` — Add mesh socket · W
- [ ] `asset.remove_socket` — Remove · W
- [ ] `asset.list_sockets` — List · R
- [ ] `asset.reload_package` — Force reload · W
- [ ] `asset.export` — Texture→PNG / Mesh→FBX · W
- [ ] `asset.search_fts` — SQLite FTS5 ranked search · R
- [ ] `asset.reindex_fts` — Rebuild FTS index · W
- [~] `asset.get_referencers` → `references_to` (Phase 2 graph; semantic match)
- [ ] `asset.set_sk_material_slots` — Skeletal mesh slots · W
- [x] `asset.diagnose_registry` → `asset.diagnose_registry`
- [x] `asset.get_mesh_bounds` → `asset.get_mesh_bounds`
- [x] `asset.get_mesh_collision` → `asset.get_mesh_collision`
- [x] `asset.move_folder` → `asset.move_folder`
- [ ] `asset.set_mesh_nav` — Nav data flags · W

**Phase 4.5 round 2** scope: import (FBX/SK/anim/texture), reimport, datatable trio, texture inspect/settings, sockets, recenter_pivot, set_mesh_material, set_sk_material_slots, set_mesh_nav, list/search/read, FTS5 (search_fts/reindex_fts), delete_batch, create_data_asset, reload_package, export. ~25 tools.

---

## level (32) — Sage covers 10

- [ ] `level.get_outliner` — List actors in level · R (similar to `get_world` actor count, but listing)
- [x] `level.place_actor` → `spawn_actor`
- [x] `level.delete_actor` → `delete_actor`
- [ ] `level.get_actor_details` — Inspect properties · R
- [x] `level.move_actor` → `set_transform` (partial)
- [x] `level.select` → `select_actors`
- [x] `level.get_selected` → `get_selected_actors`
- [x] `level.add_component` → `add_component`
- [x] `level.set_component_property` → `modify_component_property`
- [x] `level.get_current` → `get_current_level`
- [ ] `level.load` — Load by path · W
- [x] `level.save` → `save_level`
- [ ] `level.list` — List levels in directory · R
- [ ] `level.create` — New level asset · W
- [ ] `level.spawn_volume` — Place volume actor · W
- [ ] `level.list_volumes` — List by type · R
- [ ] `level.set_volume_properties` — Edit volume · W
- [ ] `level.spawn_light` — Place light · W
- [ ] `level.set_light_properties` — Intensity/color/rotation · W
- [ ] `level.set_fog_properties` — ExponentialHeightFog · W
- [ ] `level.get_actors_by_class` — Filter by class · R
- [ ] `level.count_actors_by_class` — Histogram · R
- [ ] `level.get_runtime_virtual_texture_summary` — RVT volumes · R
- [ ] `level.set_water_body_property` — WaterBodyComponent · W
- [ ] `level.build_lighting` — Build lights · W
- [ ] `level.get_spline_info` — Spline points · R
- [ ] `level.set_spline_points` — Set spline · W
- [ ] `level.set_actor_material` — Material on actor · W
- [~] `level.get_world_settings` → Sage `get_world` (partial — full settings not exposed)
- [ ] `level.set_world_settings` — Set world settings · W
- [ ] `level.get_actor_bounds` — Actor AABB · R
- [ ] `level.resolve_actor` — Internal name → editor label · R

---

## niagara (26) — Sage covers 0

- [ ] `niagara.list` · R
- [ ] `niagara.get_info` · R
- [ ] `niagara.spawn` · W
- [ ] `niagara.set_parameter` · W
- [ ] `niagara.create` · W
- [ ] `niagara.create_emitter` · W
- [ ] `niagara.add_emitter` · W
- [ ] `niagara.list_emitters` · R
- [ ] `niagara.set_emitter_property` · W
- [ ] `niagara.list_modules` · R
- [ ] `niagara.get_emitter_info` · R
- [ ] `niagara.list_renderers` · R
- [ ] `niagara.add_renderer` · W
- [ ] `niagara.remove_renderer` · W
- [ ] `niagara.set_renderer_property` · W
- [ ] `niagara.inspect_data_interfaces` · R
- [ ] `niagara.create_system_from_spec` · W
- [ ] `niagara.get_compiled_hlsl` · R
- [ ] `niagara.list_system_parameters` · R
- [ ] `niagara.list_module_inputs` · R
- [ ] `niagara.set_module_input` · W
- [ ] `niagara.list_static_switches` · R
- [ ] `niagara.set_static_switch` · W
- [ ] `niagara.create_module_from_hlsl` · W
- [ ] `niagara.create_scratch_module` · W
- [ ] `niagara.batch` · W

**Phase 4.9 milestone** — needs `Niagara` + `NiagaraEditor` modules in plugin Build.cs. Full subsystem.

---

## material (27) — Sage covers 11

- [x] `material.read` → `mat.read`
- [x] `material.list_parameters` → `mat.list_parameters`
- [~] `material.set_parameter` → `modify_material_parameter` (Phase 1)
- [ ] `material.set_expression_value` — Already in Sage as `mat.set_expression_value`. Mark as `[x]`.
- [ ] `material.disconnect_property` — Disconnect input · W
- [x] `material.create_instance` → `mat.create_instance`
- [ ] `material.create` — Create material · W
- [x] `material.set_shading_model` → `mat.set_shading_model`
- [ ] `material.set_blend_mode` — Opaque/Masked/Translucent · W
- [x] `material.set_base_color` → `mat.set_base_color`
- [x] `material.connect_texture` → `mat.connect_texture`
- [x] `material.add_expression` → `mat.add_expression`
- [x] `material.connect_expressions` → `mat.connect_expressions`
- [x] `material.connect_to_property` → `mat.connect_to_property`
- [x] `material.list_expressions` → `mat.list_expressions`
- [x] `material.delete_expression` → `mat.delete_expression`
- [ ] `material.list_expression_types` — Available types · R
- [ ] `material.recompile` — Recompile shader · W (Phase 4.3 partly; explicit fn helpful)
- [ ] `material.duplicate` — Duplicate material · W
- [x] `material.validate` → `mat.validate`
- [ ] `material.get_shader_stats` — Compile stats · R
- [ ] `material.export_graph` — Graph as JSON · R
- [ ] `material.import_graph` — Rebuild from JSON · W
- [ ] `material.build_graph` — From declarative spec · W
- [ ] `material.render_preview` — Preview PNG · W
- [ ] `material.begin_transaction` — Undo tx · W
- [ ] `material.end_transaction` — Undo tx · W

**Phase 4.3 round 2** scope: build_graph, render_preview, shader_stats, export_graph, import_graph, list_expression_types, set_blend_mode, disconnect_property, recompile, duplicate, create (base material), explicit transactions. ~12 tools.

---

## pcg (16) — Sage covers 0

- [ ] `pcg.list_graphs` · R
- [ ] `pcg.read_graph` · R
- [ ] `pcg.read_node_settings` · R
- [ ] `pcg.get_components` · R
- [ ] `pcg.get_component_details` · R
- [ ] `pcg.create_graph` · W
- [ ] `pcg.add_node` · W
- [ ] `pcg.connect_nodes` · W
- [ ] `pcg.set_node_settings` · W
- [ ] `pcg.set_static_mesh_spawner_meshes` · W
- [ ] `pcg.remove_node` · W
- [ ] `pcg.execute` · W
- [ ] `pcg.force_regenerate` · W
- [ ] `pcg.cleanup` · W
- [ ] `pcg.toggle_graph` · W
- [ ] `pcg.add_volume` · W

**Phase 4.12** — needs `PCG` + `PCGEditor` module deps.

---

## widget (17) — Sage covers 0

- [ ] `widget.read_tree` · R
- [ ] `widget.get_details` · R
- [ ] `widget.set_property` · W
- [ ] `widget.list` · R
- [ ] `widget.read_animations` · R
- [ ] `widget.create` · W
- [ ] `widget.create_utility_widget` · W
- [ ] `widget.run_utility_widget` · W
- [ ] `widget.create_utility_blueprint` · W
- [ ] `widget.run_utility_blueprint` · W
- [ ] `widget.add_widget` · W
- [ ] `widget.remove_widget` · W
- [ ] `widget.move_widget` · W
- [ ] `widget.list_classes` · R
- [ ] `widget.list_runtime` · R
- [ ] `widget.get_runtime` · R
- [ ] `widget.get_runtime_delegates` · R

**Phase 4.11** — `UMG` + `UMGEditor` module deps.

---

## gas (9) — Sage covers 0

- [ ] `gas.add_asc` · W
- [ ] `gas.create_attribute_set` · W
- [ ] `gas.add_attribute` · W
- [ ] `gas.create_ability` · W
- [ ] `gas.set_ability_tags` · W
- [ ] `gas.create_effect` · W
- [ ] `gas.set_effect_modifier` · W
- [ ] `gas.create_cue` · W
- [ ] `gas.get_info` · R

**Phase 4.15** — only useful if project uses GAS plugin.

---

## networking (11) — Sage covers 0

- [ ] `networking.set_replicates` · W
- [ ] `networking.set_property_replicated` · W
- [ ] `networking.configure_net_frequency` · W
- [ ] `networking.set_dormancy` · W
- [ ] `networking.set_net_load_on_client` · W
- [ ] `networking.set_always_relevant` · W
- [ ] `networking.set_only_relevant_to_owner` · W
- [ ] `networking.configure_cull_distance` · W
- [ ] `networking.set_priority` · W
- [ ] `networking.set_replicate_movement` · W
- [ ] `networking.get_info` · R

**Phase 4.16** — small surface; could be inlined as `actor.set_net_*` family extending Phase 1 actor mutation tools.

---

## landscape (11) — Sage covers 0

- [ ] `landscape.get_info` · R
- [ ] `landscape.list_layers` · R
- [ ] `landscape.sample` · R
- [ ] `landscape.list_splines` · R
- [ ] `landscape.get_component` · R
- [ ] `landscape.sculpt` · W
- [ ] `landscape.paint_layer` · W
- [ ] `landscape.set_material` · W
- [ ] `landscape.add_layer_info` · W
- [ ] `landscape.import_heightmap` · W
- [ ] `landscape.get_material_usage_summary` · R

**Phase 4.13** — `LandscapeEditor` module dep.

---

## foliage (7) — Sage covers 0

- [ ] `foliage.list_types` · R
- [ ] `foliage.get_settings` · R
- [ ] `foliage.sample` · R
- [ ] `foliage.paint` · W
- [ ] `foliage.erase` · W
- [ ] `foliage.create_type` · W
- [ ] `foliage.set_settings` · W

**Phase 4.14** — `FoliageEdit` module dep.

---

## reflection (6) — Sage covers 4

- [x] `reflection.reflect_class` → `reflect_class`
- [x] `reflection.reflect_struct` → `reflect_struct`
- [x] `reflection.reflect_enum` → `reflect_enum`
- [x] `reflection.list_classes` → `list_classes`
- [ ] `reflection.list_tags` — Gameplay tags · R
- [ ] `reflection.create_tag` — Create gameplay tag · W

Sage has additional reflection tools (`list_structs`, `list_enums`, `find_implementers`, `class_default_object`) not in ue-mcp's reflection category.

---

## audio (5) — Sage covers 0

- [ ] `audio.list` · R
- [ ] `audio.play_at_location` · W
- [ ] `audio.spawn_ambient` · W
- [ ] `audio.create_cue` · W
- [ ] `audio.create_metasound` · W

**Phase 4.17** — `AudioMixer`, `MetasoundEditor` module deps.

---

## feedback (1) — Sage covers 0

- [ ] `feedback.submit` — Submit gap report · W

**Decision:** Sage equivalent is `report_issue` writing to `.claude/notes/lessons.md`. Plan in Phase 4.19 reporting/observability.

---

## demo (2) — Sage covers 0

- [ ] `demo.step` · W
- [ ] `demo.cleanup` · W

**Decision:** Skip — project-specific to ue-mcp's "Neon Shrine" demo. Not generic.

---

## Recommended ordering for Sage Phase 4 round 2+

1. **Phase 4.6 round 2** — `dialog_policy` quartet (5 tools) → unblocks autonomous editor flow when UE pops up modals (saves, reloads, asset deletion confirms). **Highest leverage per tool.**
2. **Phase 4.2 round 2** — Blueprint write completion (~25 tools) → finishes the biggest gap in current authoring surface.
3. **Phase 4.5 round 2** — Asset import/datatable/textures/sockets (~25 tools) → asset pipeline becomes self-serve from the agent.
4. **Phase 4.4** — Kuzu COPY FROM perf → ingest 30s → <5s, scales with subsystem additions.
5. **Phase 4.7** — Project / engine introspection (filesystem-only, ~24 tools) → "understand the project" without booting editor.
6. **Phase 4.10** — AI / Gameplay / Input system / framework BPs → 45 tools, big surface, but each individually small.
7. **Phase 4.8** — Animation (~46 tools) → AnimBP authoring is high-value for game dev agent.
8. **Phase 4.3 round 2** + **Phase 4.6 round 2** + **Phase 4.5 round 2** parallel where module deps don't overlap.
9. **Phase 4.9** — Niagara (~26 tools) → niche; deferred unless concrete user need.
10. **Phase 4.11/4.12/4.13/4.14/4.15/4.16/4.17/4.18** — UMG / PCG / Landscape / Foliage / GAS / Networking / Audio / SCM extras → demand-driven.
11. **Phase 4.19** — `report_issue` + session log + metrics.
12. **Phase 4.20** — Headless test mode (filesystem-only fallback for CI).

Each phase still ships read+write together (no scope cuts). Time estimates are not binding — milestone graph drives the calendar, not vice versa.
