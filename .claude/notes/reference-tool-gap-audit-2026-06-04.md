# Reference Tool Gap Audit - 2026-06-04

Scope: list only. No implementation, build, package, deploy, or live editor dogfood was run.

Baseline:
- Sage source schemas: 693 tools.
- Sage plugin handlers: 675 handlers.
- Expected schema-only local tools: `jobs.list`, `jobs.get`, `jobs.wait`, `jobs.logs`, `jobs.cancel`.
- Reference clones: `C:\Users\mahmu\AppData\Local\Temp\sage-ref-audit-20260604-004937`.

References checked:
- Monolith: `tumourlove/monolith`, wiki Tool Reference, and `Docs/API_REFERENCE.md`.
- UE-MCP: `db-lyon/ue-mcp`, especially `src/tools/*.ts`.
- ChiR24: `ChiR24/Unreal_mcp`, especially `docs/handler-mapping.md`.
- Flopperam: hosted/local README tool list.
- runreal: README tool list.
- remiphilippe: `remiphilippe/mcp-unreal` source and implementation docs.
- Codeturion unreal-api-mcp: Python source and package docs.
- UECortex: listing only; exact tool list not source-verified.

## Confirmed Missing Or Weak In Sage

### 1. Tool Ergonomics, Self-Documentation, And Bulk Fill

- `monolith.guide`: sectioned AI onboarding guide.
- `describe.schema`, `describe.list_targets`, `describe.action_schema`: runtime schema/introspection for any action and target.
- `bulk_fill.apply`, `bulk_fill.list_namespaces`: generic reflected nested JSON tree writer with dry-run/strict report across domains.
- Universal response shaping: detail levels, output field projection, cursor pagination, fuzzy `did_you_mean`, schema-tagged param kinds.
- Persistent stdio-to-HTTP proxy with editor-restart survival and `notifications/tools/list_changed`.
- JSONL tool-call log/proxy log for debugging MCP sessions.

### 2. Blueprint And Dataset Authoring

- `blueprint.auto_layout` / Monolith `auto_layout`: graph auto-layout. Sage has no BP graph auto-layout.
- `blueprint.add_timeline_track`: create/update Blueprint timeline tracks and generated curve assets.
- `blueprint.set_component_override_materials`: dedicated mesh component override-material writer.
- `blueprint.set_capsule_size`: component-method based capsule resize instead of raw property writes.
- `blueprint.cleanup_graph`: orphan/corrupt node cleanup.
- `blueprint.connect_pins_batch`: batch pin wiring in one compile/save.
- `blueprint.set_node_position`: graph layout positioning.
- Monolith `add_macro`, `remove_function`, `set_function_params`, `promote_pin_to_variable`, `scaffold_interface_implementation`.
- Monolith dataset pack: DataTable row CRUD, CurveTable row CRUD, StringTable read/edit/import/export.
- Monolith `seed_data_asset`: create and populate a DataAsset atomically.
- Monolith `audit_cdo_drift`: Blueprint CDO drift audit.
- UE-MCP `asset.set_datatable_row`, `asset.add_datatable_row`, `asset.update_datatable_row`, `asset.remove_datatable_row`.
- UE-MCP `reflection.create_enum`, `reflection.set_enum_entries`.

### 3. Material And Shading

- `create_custom_hlsl_node`, `update_custom_hlsl_node`: material Custom HLSL expression authoring.
- `move_expression`, `rename_expression`, `duplicate_expression`, `replace_expression`: material graph refactor ops.
- `create_material_function`, `build_function_graph`, `get_function_info`: material function lifecycle.
- `get_instance_parameters`, `set_instance_parameter`, `set_instance_parameters`, `set_instance_parent`, `clear_instance_parameter`, `list_material_instances`: full material instance override lifecycle.
- `batch_set_material_property`, `batch_recompile`: material batch ops.
- `get_thumbnail`: base64/file thumbnail readback.
- `capture_material_grid`: side-by-side material instance render.
- `capture_with_overlay`: debug-view overlays such as wireframe, normals, UV density, lightmap density, shader complexity.
- `inspect_material_pbr`: PBR/ORM/ARM/MRA channel-packing classifier.
- `inspect_texture_channels`: per-channel texture statistics and optional split images.
- `audit_orphan_materials`: material orphan/reference audit.

### 4. Audio, SoundCue, MetaSound

- Sound asset writers: `create_sound_attenuation`, `set_attenuation_settings`, `create_sound_class`, `set_sound_class_properties`, `create_sound_mix`, `set_sound_mix_settings`, `create_sound_concurrency`, `set_concurrency_settings`, `create_sound_submix`, `set_submix_properties`.
- Audio search/health: `search_audio_assets`, `find_audio_references`, `find_unused_audio`, `find_sounds_without_class`, `find_unattenuated_sounds`, `get_audio_stats`.
- Audio batch ops: `batch_assign_sound_class`, `batch_assign_attenuation`, `batch_set_compression`, `batch_set_submix`, `batch_set_concurrency`, `batch_set_looping`, `batch_set_virtualization`, `batch_rename_audio`, `batch_set_sound_wave_properties`, `apply_audio_template`.
- Sound Cue graph authoring: `get_sound_cue_graph`, `add_sound_cue_node`, `remove_sound_cue_node`, `connect_sound_cue_nodes`, `set_sound_cue_first_node`, `set_sound_cue_node_property`, `list_sound_cue_node_types`, `validate_sound_cue`, `build_sound_cue_from_spec`.
- Sound Cue templates: `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`.
- Audio preview: `preview_sound`, `stop_preview`, `get_sound_cue_duration`.
- MetaSound graph authoring: `create_metasound_source`, `create_metasound_patch`, `add_metasound_node`, `remove_metasound_node`, `connect_metasound_nodes`, `disconnect_metasound_nodes`, `add_metasound_input`, `add_metasound_output`, `set_metasound_input_default`, `add_metasound_interface`, `build_metasound_from_spec`.
- MetaSound read/discovery: `get_metasound_graph`, `list_metasound_connections`, `list_available_metasound_nodes`, `get_metasound_node_info`, `find_metasound_node_inputs`, `find_metasound_node_outputs`, `get_metasound_input_names`.
- MetaSound templates: `create_metasound_preset`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound`, `add_metasound_variable`, `set_metasound_node_location`.
- AI/audio bridge: `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`.

### 5. AI, Behavior Tree, Blackboard, EQS, Smart Objects

- Blackboard CRUD beyond create/read basics: `get_blackboard`, `delete_blackboard`, `duplicate_blackboard`, `add_bb_key`, `remove_bb_key`, `rename_bb_key`, `get_bb_key_details`, `batch_add_bb_keys`, `set_bb_parent`, `compare_blackboards`.
- UE-MCP blackboard actions: `gameplay.add_blackboard_key`, `gameplay.remove_blackboard_key`, `gameplay.set_blackboard_parent`, `gameplay.read_blackboard`.
- Behavior Tree structural authoring: `delete_behavior_tree`, `duplicate_behavior_tree`, `set_bt_blackboard`, `add_bt_node`, `remove_bt_node`, `move_bt_node`, `add_bt_decorator`, `remove_bt_decorator`, `add_bt_service`, `remove_bt_service`, `set_bt_node_property`, `get_bt_node_properties`, `reorder_bt_children`.
- Behavior Tree special tasks/specs: `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`, `build_behavior_tree_from_spec`, `export_bt_spec`, `import_bt_spec`, `clone_bt_subtree`, `auto_arrange_bt`, `compare_behavior_trees`, `create_bt_task_blueprint`, `create_bt_decorator_blueprint`, `create_bt_service_blueprint`, `generate_bt_diagram`, `get_bt_graph`.
- EQS deep authoring: `get_eqs_query`, `delete_eqs_query`, `duplicate_eqs_query`, `add_eqs_generator`, `remove_eqs_generator`, `configure_eqs_generator`, `add_eqs_test`, `remove_eqs_test`, `configure_eqs_test`, `configure_eqs_scoring`, `configure_eqs_filter`, `list_eqs_generator_types`, `list_eqs_test_types`, `list_eqs_contexts`, `validate_eqs_query`, `reorder_eqs_tests`, `build_eqs_query_from_spec`, `create_eqs_from_template`.
- Smart Object deep authoring: `get_smart_object_definition`, `list_smart_object_definitions`, `delete_smart_object_definition`, `add_so_slot`, `remove_so_slot`, `configure_so_slot`, `add_so_behavior_definition`, `remove_so_behavior_definition`, `set_so_tags`, `place_smart_object_actor`, `find_smart_objects_in_level`, `validate_smart_object_definition`, `create_so_from_template`, `duplicate_smart_object_definition`.
- UE-MCP Smart Object slot actions: `gameplay.add_smart_object_slot`, `gameplay.set_smart_object_slot`, `gameplay.remove_smart_object_slot`, `gameplay.list_smart_object_slots`, `gameplay.add_smart_object_slot_behavior`.
- Runtime AI debug: `runtime_get_bb_value`, `runtime_set_bb_value`, `runtime_clear_bb_value`, `runtime_get_bt_state`, `runtime_start_bt`, `runtime_stop_bt`, `runtime_get_bt_execution_path`, `runtime_get_perceived_actors`, `runtime_check_perception`, `runtime_report_noise`, `runtime_get_st_active_states`, `runtime_send_st_event`, `runtime_find_smart_objects`, `runtime_run_eqs_query`.
- AI scaffolds: `scaffold_complete_ai_character`, `scaffold_perception_to_blackboard`, `scaffold_team_system`, `scaffold_patrol_investigate_ai`, `scaffold_enemy_ai`, `scaffold_eqs_move_sequence`, `scaffold_ai_controller_blueprint`, `scaffold_companion_ai`, `scaffold_boss_ai`, `scaffold_ambient_npc`, `scaffold_horror_stalker`, `scaffold_stealth_game_ai`, `scaffold_group_coordinator`, `scaffold_flying_ai`.
- AI validation/lint: `batch_validate_ai_assets`, `validate_ai_controller`, `get_ai_overview`, `list_ai_node_types`, `search_ai_assets`, `validate_ai_data_flow`, `find_eqs_references`, `find_so_references`, `lint_behavior_tree`, `lint_state_tree`, `detect_ai_circular_references`, `export_ai_manifest`, `get_ai_behavior_summary`.
- Mass Entity: `list_mass_entity_configs`, `get_mass_entity_config`, `create_mass_entity_config`, `add_mass_trait`, `remove_mass_trait`, `list_mass_traits`, `list_mass_processors`, `validate_mass_entity_config`, `get_mass_entity_stats`.
- ZoneGraph: `list_zone_graphs`, `query_zone_lanes`, `get_zone_lane_info`.

### 6. StateTree Deep Authoring

Sage only has create/list/component-level StateTree tools. Missing UE-MCP/Monolith surface:

- `statetree.read`, `statetree.list_states`, `statetree.add_state`, `statetree.remove_state`, `statetree.set_state_property`, `statetree.clear_state_nodes`.
- `statetree.add_task`, `statetree.remove_task`, `statetree.set_task_property`, `statetree.set_task_instance_property`.
- `statetree.add_enter_condition`, `statetree.remove_enter_condition`, `statetree.add_transition`, `statetree.remove_transition`, `statetree.add_transition_condition`.
- `statetree.add_binding`, `statetree.remove_binding`, `statetree.list_bindings`.
- `statetree.add_evaluator`, `statetree.remove_evaluator`, `statetree.set_evaluator_property`, `statetree.set_evaluator_instance_property`.
- `statetree.add_global_task`, `statetree.remove_global_task`, `statetree.set_global_task_property`, `statetree.set_global_task_instance_property`.
- `statetree.list_colors`, `statetree.add_color`.
- `statetree.list_state_parameters`, `statetree.add_state_parameter`, `statetree.remove_state_parameter`, `statetree.set_state_parameter`, `statetree.set_root_parameters`.
- `statetree.compile`, `statetree.validate`.
- Monolith spec tools: `build_state_tree_from_spec`, `export_st_spec`, `generate_st_diagram`, `auto_arrange_st`, `set_st_schema`, `list_st_task_types`, `list_st_condition_types`, `get_st_bindable_properties`, `add_st_consideration`, `configure_st_consideration`, `list_st_extension_types`, `add_st_extension`.

### 7. Mesh, GeometryScript, ProceduralMesh, RealtimeMesh

- Mesh inspection beyond Sage basics: `get_mesh_lods`, `get_mesh_uvs`, `analyze_skeletal_mesh`, `analyze_mesh_quality`, `compare_meshes`, `get_vertex_colors`.
- GeometryScript ops: `mesh_boolean`, `mesh_simplify`, `mesh_remesh`, `mesh_mirror`, `mesh_fill_holes`, `compute_uvs`, `fix_mesh_quality`.
- Collision and LOD writers: `generate_collision`, `generate_lods`, `set_lod_screen_sizes`, `set_collision_preset`, `set_mesh_collision`, `auto_generate_lods`, `generate_proxy_mesh`, `setup_hlod`.
- ChiR Geometry actions: `boolean_union`, `boolean_subtract`, `boolean_intersection`, `boolean_trim`, `remesh_uniform`, `remesh_voxel`, `remove_degenerates`, `auto_uv`, `unwrap_uv`, `pack_uv_islands`, `project_uv`, `transform_uvs`, `generate_complex_collision`, `simplify_collision`, `set_lod_settings`.
- remiphilippe `procedural_mesh`: `create_section`, `update_section`, `clear`, `set_material` on `UProceduralMeshComponent`.
- remiphilippe `realtime_mesh`: `create_lod`, `create_section_group`, `create_section`, `update_mesh_data`, `set_material_slot`, `setup_collision` for RealtimeMeshComponent.
- Flopperam `chaos_edit`: Geometry Collection/destruction editing.

### 8. Level, Actor, Spatial, World-Building

- Spatial queries: `line_trace`, `raycast`, `overlap_test`, `radial_sweep`, `line_of_sight`, `navigation_raycast`, `find_path`, `test_path`, `get_random_navigable_point`.
- UE-MCP actor/level ops: `level.snap_actor_to_floor`, `level.get_relative_transform`, `level.read_actor_motion`, `level.add_actor_tag`, `level.remove_actor_tag`, `level.set_actor_tags`, `level.list_actor_tags`, `level.attach_actor`, `level.detach_actor`, `level.set_actor_mobility`.
- Streaming/edit level ops: `level.get_current_edit_level`, `level.set_current_edit_level`, `level.list_streaming_sublevels`, `level.add_streaming_sublevel`, `level.remove_streaming_sublevel`, `level.set_streaming_sublevel_properties`.
- Batch placement: `level.spawn_grid`, `level.batch_translate`, `level.place_actors_batch`, Monolith `scatter_props`, `replace_blockout_with_assets`, `export_layout`, `import_layout`.
- Flopperam local world-building tools: `create_town`, `construct_house`, `construct_mansion`, `create_tower`, `create_arch`, `create_staircase`, `create_castle_fortress`, `create_suspension_bridge`, `create_aqueduct`, `create_maze`, `create_pyramid`, `create_wall`.
- Monolith procedural geometry/world tools: `create_parametric_mesh`, `create_horror_prop`, `create_structure`, `create_building`, `create_maze`, `create_pipe_network`, `create_fragments`, `create_terrain_patch`.
- Monolith town-gen pipeline: `generate_floor_plan`, `create_building_from_grid`, `generate_facade`, `generate_roof`, `register_building`, `create_city_block`.
- Decal/detail placement: path-following decal placement and storytelling presets.

### 9. Landscape, Foliage, PCG

- Landscape creation/import/export: UE-MCP `landscape.create`; Flopperam `landscape_edit` includes heightmap import/export and semantic features. Sage has sculpt/import_heightmap but no full terrain feature pipeline.
- Foliage instance-level operations: `paint_foliage`, `add_foliage_instances`, `get_foliage_instances`, `remove_foliage`, `foliage_inspect`, `foliage_edit`.
- PCG graph import/export and advanced authoring: UE-MCP `pcg.export_graph`, `pcg.import_graph`, `pcg.set_static_mesh_spawner_meshes`; Flopperam `pcg_graph_edit` with generators/samplers/filters/mesh spawners.

### 10. UMG, CommonUI, MVVM, UI Templates

- UI scaffolders/templates: HUD, main menu, settings panel with tabs, pause menu, dialog, loading screen, inventory grid, save/load menu, toast notifications.
- `build_ui_from_spec`, `dump_ui_spec_schema`, `dump_ui_spec`: schema-driven UI spec builder/serializer.
- CommonUI actions: activatable stacks, focus, button conversion, dialogs, lists, navigation, styles, input actions.
- Close-the-loop UI primitives: `rename_widget`, `add_widget_variable`, `audit_focus_chain`, `apply_token_binding`, `list_widget_property_enums`, `convert_textblock_to_common`, `convert_border_to_common`, `set_action_bar_button_class`, `dump_blueprint_compile_log`, `reparent_widget_root`, `set_widget_is_variable`.
- Navigation: `set_widget_navigation_bulk`, `dump_widget_navigation`.
- Accessibility audit for UI.
- GAS UI binding aliases: bind widgets to GAS attributes/effects/tags.
- Flopperam `widget_inspect`, `widget_edit`: widget tree, styles, animation, MVVM, event binding.

### 11. GAS And Gameplay Tags

- Monolith GAS ability ops: grant/remove abilities, inspect specs, configure activation policy, ability tasks, input binding, ability scaffolds.
- GameplayEffect deep authoring: executions, modifiers, duration, stacking, immunity, granted tags, cues, cooldown/cost patterns.
- AttributeSet lifecycle: Blueprint AttributeSet generation, DataTable initialization, default rows, attribute metadata.
- ASC runtime/debug: grant ability to pawn, apply/remove effects, inspect active effects, query tags/attributes/spec handles.
- GameplayCue graph/template tools.
- Targeting: target-data helpers, hit/trace targeting, target actor authoring.
- Tags: full registry edit, category/tag validation, redirect cleanup.
- Flopperam `tag_registry_edit`; remiphilippe `gas_ops`.

### 12. Animation, Rigging, Cloth, Physics Asset

Sage is strong here, but reference-only gaps remain:

- Aim Offset lifecycle and sample editing: `create_aim_offset`, `add_aim_offset_sample`.
- Pose Library: `create_pose_library`.
- Compatible skeleton management: `add_compatible_skeleton`, `remove_compatible_skeleton`, `get_compatible_skeletons`.
- Animation preview: `preview_animation` and screenshot/capture of posed skeletal mesh.
- Physics asset authoring: `create_physics_asset`, `add_physics_body`, `configure_physics_body`, `add_physics_constraint`, `configure_constraint_limits`, `list_physics_bodies`.
- Cloth: `assign_cloth_asset_to_mesh`, `bind_cloth_to_skeletal_mesh`.
- Skin weights: `auto_skin_weights`, `copy_weights`, `mirror_weights`, `normalize_weights`, `prune_weights`, `set_vertex_weights`.
- Morph targets: `create_morph_target`, `import_morph_targets`, `set_morph_target_deltas`.
- Control Rig graph/RigVM ops: `add_rig_unit`, `connect_rig_elements`; Sage currently has hierarchy/control operations but not full RigVM graph authoring.
- Animation graph layout: `animation.auto_layout`.

### 13. Niagara

Tracked separately in `.claude/notes/niagara-mcp-reference-matrix.md`, but still in the global missing list:

- `get_custom_hlsl_text`, `set_custom_hlsl_text`.
- HLSL module/function/scratch authoring that actually constructs graph nodes.
- Dynamic input lifecycle: search, attach, configure sub-inputs, read tree, remove.
- Event handler and simulation stage creation with usage selectors.
- Search/discovery pack: `search_by_parameter`, `search_by_data_interface`, `search_by_material`, `query_niagara`, `find_similar_systems`, `find_niagara_references`, `list_system_data_interfaces`.
- Renderer binding/material convenience.
- Data-interface add/configure helpers.
- Preview image/GIF capture.
- Stateless emitter factory and temporal composite writers where Sage coverage is partial.

### 14. Sequencer And Cinematics

- Monolith `level_sequence_query`: create/manage tracks, shots, bindings, SQLite sequence index.
- Flopperam `sequencer_edit`: camera cuts and richer sequence editing.
- Sage has minimal `seq.create`, `seq.add_track`, `seq.add_keyframe`, `seq.add_spawnable`, `seq.add_possessable`; missing full shot/camera-cut/binding/channel/editor workflows.

### 15. Asset, Content Browser, Marketplace, Fab

- Folder lifecycle: `asset.create_folder`, `asset.delete_folder`, `asset.move_folder`.
- Save lifecycle: `asset.save`, `asset.save_all_dirty`.
- Asset import pipeline: `asset.create_interchange_pipeline`, `asset.import_texture_batch`, `asset.read_import_sources`.
- Asset health/reporting: `asset.health_check`, `asset.generate_report`, `asset.create_thumbnail`, `asset.validate`, `asset.set_tags`.
- Fab/Marketplace: remiphilippe `fab_ops` for marketplace cache/import.
- ISM/HISM: remiphilippe `ism_ops`; UE-MCP `level.add_hismc_instances`.

### 16. C++ Source, API Docs, Reflection Intelligence, Network Audit

- API lookup: unreal-api-mcp `search_unreal_api`, `get_by_fqn`, `get_class_members`, `get_class_reference`, `get_function_signature`, `get_include_path`, `search_deprecated`, `get_deprecation_warnings`.
- Remiphilippe docs: `lookup_docs`, `lookup_class` over UE 5.7 API docs, RealtimeMesh docs, and project docs.
- Source call graph: Monolith `find_callers`, `find_callees`, richer symbol/member index; Sage has source search/read but not a full call graph.
- `source.audit_module_dep_reality`: Build.cs dependency audit based on reflected type references.
- Decision intelligence: `decision.list_decisions`, `decision.get_decision`, `decision.list_stale`, `decision.find_supersession_chain`, `decision.find_referent_decisions`.
- Risk intelligence: `risk.get_hotspot_score`, `risk.get_cochange_pairs`, `risk.get_file_churn`, `risk.get_release_window_hotspots`, `risk.list_conditional_gates`.
- C++ reflection index: `cppreflect.get_uclass`, `cppreflect.list_uproperties`, `cppreflect.list_ufunctions`, `cppreflect.find_interface_impls`, `cppreflect.find_class_specifier`, `cppreflect.list_class_specifiers`, `reflect.rebuild_reflection_index`.
- Network audit: `network.list_replicated_classes`, `network.list_rpc_functions`, `network.list_onrep_handlers`, `network.audit_unbalanced_onreps`.
- Pipeline composers: `pipeline.pr_review`, `pipeline.release_readiness`.

### 17. Runtime Verification, Testing, Capture, Performance

- Visual tests: remiphilippe `run_visual_tests`.
- Structured test logs: remiphilippe `get_test_log`.
- Viewport/window capture variants: `window_capture`, `capture_viewport`, `capture_scene_preview` for static mesh/skeletal mesh/widget/material/Niagara, GIF capture.
- Runtime verification harnesses: Flopperam `pie_test_bp`, `pie_test_scene` with assertion packs.
- UE automation: Monolith `list_automation_tests`, `run_automation_tests`.
- Build/project: `build_project`, `cook_project`, `live_compile` as first-class headless/editor tools with structured output; Sage has some overlapping build/cook hooks but not the full remiphilippe/Monolith diagnostic shape.
- Performance: `performance_audit`, `generate_memory_report`, `configure_texture_streaming`, `start_profiling`, `stop_profiling`, `show_fps`, `show_stats`, `set_resolution_scale`, `set_vsync`, `set_frame_rate_limit`, `configure_nanite`, `configure_lod`.
- Monolith spatial/performance analysis: overdraw hotspots, shadow cost, frustum culled triangle budgets, region budgets, pre-spawn cost estimates.

### 18. Specialized Marketplace / Plugin Domains

- Logic Driver Pro: `logicdriver_query` 66 actions: state machine CRUD, graph read/write, runtime PIE, JSON spec, scaffolding, components, text graph, discovery.
- ComboGraph: `combograph_query` 13 actions: combo graph CRUD, nodes, edges, effects, cues, ability scaffolding, runtime/editor graph sync.
- Inventory/sibling plugin adapters in Monolith are not public release baseline, but the architecture supports adapter-style extension.
- UECortex exact tool names were not source-verified; listing claims 139 pure C++ tools across 13 categories including Niagara, so keep it as a watch target only.

## Lower Priority Or Mostly Covered

- runreal's Python Remote Execution surface is mostly covered by Sage `editor.run_python`, but runreal's low-install "no custom plugin" positioning is useful.
- runreal `set_unreal_engine_path`, `get_unreal_engine_path`, `set_unreal_project_path`, `get_unreal_project_path` are configuration conveniences; Sage has project routing but not the same explicit engine-path setter/getter tools.
- remiphilippe `status` has nice project/editor/UE path health shape; Sage has `ping`, `list_editors`, `project.get_info`, but not one unified health payload.

## Next Conversion Step

Turn this list into gap batches, newest first:

1. AI/BT/Blackboard/EQS/StateTree/SmartObject.
2. Mesh/GeometryScript/ProceduralMesh/RealtimeMesh.
3. Audio/SoundCue/MetaSound.
4. UI/CommonUI/templates/accessibility.
5. Material instance/function/HLSL/inspection.
6. C++ API/reflection/network/risk/decision intelligence.
7. Runtime verification/capture/performance.
8. Asset/folder/DataTable/Fab/ISM/HISM.
9. Niagara remaining tranche.
