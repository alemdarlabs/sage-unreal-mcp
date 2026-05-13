# Schema fixes inbox

Append-only. Each `### file/cluster: handler_name` block lists schema changes needed in `server/src/tools/phase4_schemas.cpp` (or wherever schema lives). The user consolidates.

---

## Agent 1/8 — SageAnimationTools.cpp

### Cluster B: add_play_montage_notify_window → add_slot_node
- Rename tool: `animation.add_play_montage_notify_window` → `animation.add_slot_node`.
- Update DisplayName to "Slot".
- Description: "Spawn a UAnimGraphNode_Slot in the AnimGraph (Montage slot routing). Returns node_id."
- Handler is renamed in plugin code (`AddPlayMontageNotifyWindowImpl` → `AddSlotNodeImpl`); registry key: `animation.add_slot_node`.

### Cluster C ek: add_state_alias
- Add params: `aliased_states: string[]` (FGuid string list of state ids the alias represents) and `global_alias: bool` (default false).
- Without these, BP compile fails — see plugin handler `AddStateAliasImpl` change.

### Cluster E: blendspace stub descriptions
- `animation.set_blendspace_axis` description: prefix with "[STUB — pending implementation] ".
- `animation.set_blendspace_smoothing` description: prefix with "[STUB — pending implementation] ".
- `animation.set_blendspace_target_weight_interpolation` description: prefix with "[STUB — pending implementation] ".

### Cluster J: character.play_root_motion_source
- Add `source_type` enum entries: `RadialForce`, `MoveToForce`.
- Add params (optional, validated by source_type):
  - `location: number[3]` — RadialForce world-space center
  - `radius: number` — RadialForce influence radius
  - `target_location: number[3]` — MoveToForce destination
  - `path_offset_curve: string` (UCurveVector path) — JumpForce curve
  - `time_mapping_curve: string` (UCurveFloat path) — JumpForce time mapping
  - `sensitive_liftoff_check: bool` (default true) — JumpForce sensitive liftoff toggle

### Cluster J: character.remove_root_motion_source (NEW tool)
- Register new tool: `character.remove_root_motion_source`.
- Params: `actor: string` (PIE actor path), `source_id: number` (uint16 returned by play_root_motion_source).
- Description: "Cancel a running root-motion source by ID on a Character's CMC. PIE-only."

### Cluster J: character.play_montage
- Add optional param `start_position: number` (seconds; if set, calls `Montage_SetPosition(...)` post-Play).

### Cluster J: character.stop_montage
- Add optional param `montage: string` (UAnimMontage path; if set, only stops that specific montage instead of all).

### Eski tools — schema/handler param rename audit
Plugin handler reads renamed to match SCHEMA names below (more stable for clients). Schema names confirmed canonical — no schema change needed for these unless schema currently differs:
- `animation.list_skeletal_meshes`: schema MUST declare `skeleton` (not `skeleton_path`).
- `animation.create_anim_blueprint`: schema MUST declare `skeleton` (not `skeleton_path`).
- `animation.create_blendspace`: schema MUST declare `skeleton` and `dimensions: integer (1|2, default 2)` instead of `type: "1D"|"2D"`.
- `animation.create_sequence`: schema MUST declare `skeleton`.
- `animation.create_composite`: schema MUST declare `skeleton`.
- `animation.set_anim_blueprint_skeleton`: schema MUST declare `skeleton`.
- `animation.read_bone_track`: schema MUST declare `bone` (not `bone_name`).
- `animation.add_virtual_bone` (stub): schema MUST declare `skeleton`, `source_bone`, `target_bone`, `name` (not `path`/`bone_name`/`parent_name`/`target_name`).
- `animation.set_montage_properties`: schema MUST declare `blend_in` and `blend_out` (not `blend_in_time`/`blend_out_time`).
- `animation.set_montage_slot`: schema MUST declare `slot` (not `slot_name`). Add optional `slot_index: integer` (default 0).
- `animation.create_ik_retargeter`: schema MUST declare `source_ik_rig` and `target_ik_rig` (not `_path` suffix).
- `animation.create_montage`: schema should declare optional `sequence_path: string` (currently undeclared but handler reads it).

### Cluster I: add_slot
- Schema currently declares `slot` (per finding #65 note). Plugin handler reads `slot_name` (legacy). Either: (a) rename schema to `slot_name`, or (b) rename handler. Decision deferred — current code reads `slot_name`.
- New response field: `registered: bool` (true on first slot registration, false if slot already known).

### Cluster I: add_skeleton_socket
- Add optional param `transform: { location: number[3], rotation: number[3] (pitch,yaw,roll), scale: number[3] }` — plugin now applies these to RelativeLocation/Rotation/Scale on the spawned USkeletalMeshSocket.
- Behaviour: parent bone now validated (rejects -32602 if not in ref skeleton); duplicate socket name rejected (-32602).
- New response field: `parent_bone: string`.

### Cluster I: remove_skeleton_socket
- New response field: `removed_count: integer` (alongside existing `removed: bool`).

### Cluster A: list_animgraph_nodes
- Response field change: `is_anim_graph_node: bool` REMOVED.
- New response field per node: `kind: enum("state_machine"|"asset_player"|"blend_list"|"bone_control"|"anim_node"|"other")`.

### Cluster C ek: list_states
- New response field per state: `kind: enum("state"|"alias"|"conduit")`.

### Cluster C ek: list_transitions
- New response fields per transition: `from_state_id: string` (FGuid), `to_state_id: string`, `bidirectional: bool`.

### Cluster D ek: list_notifies
- New response field per notify: `kind: enum("notify"|"notify_state"|"native")`.

### Cluster D ek: add_notify_track
- New optional response field: `_warning: string` ("duplicate track name") emitted when a track with the same name already exists.

### Cluster F: add_sync_marker
- Behavior: time bounds clamped to `[0, GetPlayLength()]` — out-of-range rejected -32602.
- New optional response field: `_warning: string` ("duplicate marker name") when an existing marker shares the name.

### Cluster E: set_blendspace_samples
- Response shape change: `skipped` array entries now structured objects `{index: int, reason: enum("not_object"|"missing"|"unresolved"), animation?: string}` instead of bare strings.

### Cluster E: add_blendspace_sample
- For 1D blendspaces (UBlendSpace1D) the Y component is force-zeroed in stored sample + response echo.
- New optional response field: `_warning: string` ("both 'x' and 'position' provided; 'x' wins") when caller supplies both forms.

### Cluster A: read_blendspace_samples
- Behavioural: now applies RejectIfPie like sibling read handlers (no schema change).

### Cluster A: set_anim_node_property
- Behavioural: rejects -32602 when `value: null` AND target prop is non-FObjectProperty (silent-coercion guard).

### Cluster A: set_animgraph_root_pose
- Behavioural: rejects -32602 when `graph_name` resolves to a UAnimationTransitionGraph.

### Cluster A: connect_pose_pin / disconnect_pose_pin
- Behavioural: switched from MarkBlueprintAsStructurallyModified → MarkBlueprintAsModified.

### Cluster A: bind_anim_node_property
- Behavioural: short-circuits with -32601 BEFORE asset registry resolution.

### Cluster A: ResolveAnimGraphTarget extension
- The `graph_name` argument now also accepts state names (matched on UAnimStateNodeBase::GetStateName() or BoundGraph FName). Schema descriptions should note "graph_name accepts AnimGraph | state machine sub-graph FName | state name".

### Cluster B: add_sequence_player
- Behaviour: `loop` and `rate` arguments are now actually applied (writes Node.bLoopAnimation + Node.PlayRate). Previously no-op.

### Cluster B: add_blendspace_player
- Behaviour: rejects -32602 when `blendspace` arg resolves to a non-UBlendSpace asset.

### Cluster B: spawn_by_class_path-derived (add_blend_list_by_*, add_layered_blend_per_bone, add_apply_additive, add_two_bone_ik, add_skeletal_control_node, add_slot_node, add_link_anim_layer)
- Behaviour: now rejects -32602 if the target class has CLASS_Abstract or CLASS_Deprecated flags.

### Cluster B: add_state_machine_node
- Behaviour: SM-reuse path no longer creates an orphan stub graph.

### Cluster C: create_state_machine + add_state
- Behaviour: graph rename now uses RenameGraphWithSuggestion + FNameValidatorFactory (engine canonical) — collisions get suffix instead of overwriting.

### Cluster C: set_state_animation
- Behaviour: `loop` argument is now actually applied.

### Cluster C ek: add_state_alias
- Behavioural: now sets bGlobalAlias + populates AliasedStateNodes from the `aliased_states` array.
- New response fields: `global_alias: bool`, `aliased_count: integer`.

### Cluster C: add_transition
- Error message itemizes which of the 4 pins (from_out/t_in/t_out/to_in) was missing.

### Cluster H: set_montage_section_loop / set_montage_section_next
- New optional response field: `_warning: string` ("no section named X") when no section matches.

### Cluster J: character.play_root_motion_source — RadialForce + MoveToForce
- Implemented: RadialForce (`location: number[3]`, `radius: number`) + MoveToForce (`target_location: number[3]`, optional `start_location: number[3]`).
- JumpForce optional curves: `path_offset_curve: string` (UCurveVector path), `time_mapping_curve: string` (UCurveFloat path).
- New optional `sensitive_liftoff_check: bool` (default true) for ConstantForce + JumpForce.

### animation.get_bone_transforms — handler/schema MISMATCH FIX
- Handler now reads `skeleton: string` + `bones: string[]` (matches schema). Was reading legacy `path` + `bone_name` (broken).
- Response shape: `{ skeleton, count, bones: [{bone, found, location?, rotation?, scale?}] }`. Old top-level `bone_name`/`location`/etc REMOVED.

### animation.read_bone_track — `time` arg semantics
- Handler still returns ref-pose only (does not honor `time` arg — IAnimationDataModel::EvaluateBoneTrack pending). Recommend either dropping `time` from schema OR documenting the limitation in description.

### Notes — stub error message wording (no schema change, code-only)
The following stubs had their error message prefix changed inside plugin to `"[NOT IMPLEMENTED] "` (was "pending" / "DEPRECATED"). Schema descriptions may want matching `[STUB]` prefix:
- animation.set_curve_compression
- animation.run_animation_modifier
- animation.add_animation_modifier
- animation.create_anim_layer_interface
- animation.add_layer_function
- animation.implement_anim_layer_interface
- animation.set_linked_anim_layer
- animation.list_implemented_layers
- animation.set_sequence_compression_scheme
- animation.set_montage_blend_curve
- animation.copy_animation_curves
- animation.set_transition_rule
- animation.set_state_entered_event
- animation.set_bone_keyframes
- animation.set_montage_sequence
- animation.bake_root_motion_from_bone
- animation.set_pose_search_schema
- animation.add_pose_search_sequence
- animation.build_pose_search_index

---

## Agent 4/8 — SageLevelTools / SageActorTools / SageGameplayTools / SageTransactionTools

### gameplay.set_world_game_mode (phase4_schemas.cpp:~898) — Lyra Gap #15
- Rename param: `game_mode` → `game_mode_class` (UE convention: `TSubclassOf<AGameModeBase>`; response field already named `game_mode_class`; handler reads `game_mode_class`).
- Add param: `confirmed: bool` (required:true). Destructive op gate per production caution rule.
- Updated required list: `["game_mode_class","confirmed"]`.

### level.create (phase4_schemas.cpp:~1388) — Lyra Gap #14
- Add param: `confirmed: bool` (required:true). Now uses real `UWorld::CreateWorld` pipeline that registers FXSystem/Niagara — destructive (writes a new UWorld asset).
- Updated required list: `["path","confirmed"]`.

### level.load (phase4_schemas.cpp:~1378)
- Add optional param: `discard_unsaved: bool` (default false). When current world is dirty the handler now rejects with -32602 unless this is set — mirrors editor's "Save before opening?" dialog.

### delete_actor (server/src/main.cpp:~252)
- Add param: `confirmed: bool` (required:true). Production caution gate for destructive op.
- Update inputSchema required list to include `confirmed`.

### gameplay.add_imc_mapping (phase4_schemas.cpp:~710)
- Document new response field: `mapping_index: number` — index of the mapping just appended (use for subsequent set_mapping_modifiers / remove_imc_mapping calls without re-listing). No schema-side type fix needed if `additionalProperties:true`; otherwise extend the response schema.

### world.export (phase4_schemas.cpp — locate by name)
- Document new optional response field: `_perf_warning: string` — emitted only when `include_actor_props=true` and >0 actors returned. Recommends `simplify=stripped` or disabling `include_actor_props`.

### Param-match audit (4 files vs schema)
- `gameplay.set_world_game_mode`: handler reads `game_mode_class`, schema declares `game_mode` → MISMATCH (Gap #15, fix above).
- All other gameplay/level/actor/transaction handlers reviewed — no other param-name drift detected. (`level.load` `path`, `level.create` `path`, `delete_actor` `actor_id`, `set_transform` `actor_id+location+rotation`, `set_visibility` `actor_id+hidden`, transaction `tx_id`/`label` all match.)

### GameThread marshal audit
- `SageLevelTools.cpp`: `GT(...)` wrapper at registration — every handler marshalled.
- `SageGameplayTools.cpp`: `GT(...)` wrapper at registration — every handler marshalled.
- `SageActorTools.cpp`: explicit `RunOnGameThread([Args]() { ... })` per handler (`SpawnActorHandler`, `DeleteActorHandler`, `SetTransformHandler`, `SetVisibilityHandler`, `ModifyActorPropertyHandler`) — all 5 covered, no bare handlers registered.
- `SageTransactionTools.cpp`: explicit `RunOnGameThread(...)` per handler (`BeginTransactionHandler`, `CommitTransactionHandler`, `RollbackTransactionHandler`, `GetActiveTransactionsHandler`) — all 4 covered.
- No drift detected.

---

## Agent 2/8 — SageBlueprintTools.cpp

### bp.delete_variable (server/src/main.cpp ~line 1159)
- Add optional `confirmed` boolean property. Handler now rejects -32602
  unless `confirmed:true` (destructive-op gate per production discipline).
- Description bullet: "Pass `confirmed:true` to proceed."

### bp.delete_function (server/src/main.cpp ~line 1865)
- Add optional `confirmed` boolean property. Same gate as
  `bp.delete_variable`.
- Description bullet: "Pass `confirmed:true` to proceed."

### bp.full_dump (phase4_schemas.cpp ~line 1988)
- `output_path` is now sandbox-confined to `FPaths::ProjectDir()`.
  Absolute paths outside the project AND `..` segments that would escape
  ProjectDir are rejected -32602
  ("output_path must resolve inside project directory").
- Description note: "When set, output_path resolves under the project
  directory; absolute paths and `..` escapes outside the project are
  rejected."
- Response now contains additional optional skip-reason strings when a
  sub-fetch fails: `variables_skip_reason`, `components_skip_reason`,
  `graphs_skip_reason`, `event_dispatchers_skip_reason`,
  `cdo_skip_reason`, `dependencies_skip_reason` (matching the existing
  per-function `t3d_skip_reason` pattern).

### bp.import_nodes_t3d (server/src/main.cpp ~line 1630)
- Response now includes optional `_warning: string` when the T3D blob
  contained more node entries (counted via "Begin Object Class=" markers)
  than UE actually pasted — surfaces the silent entry/return dedup that
  UE applies vs the destination graph.
- Description bullet: "When the destination graph already has
  entry/return nodes, those nodes in the T3D are silently dropped; a
  `_warning` field is emitted with the dropped count."

### bp.get_cdo_properties (server/src/main.cpp ~line 1462)
- Response now includes:
  - `components: array<{name: string, class: string,
                        properties_summary: {property_names: string[],
                                             property_count: integer}}>`
  - `component_count: integer`
- These surface native CDO subobjects (e.g. `ACharacter::CharacterMovement`,
  `ACharacter::CharacterMesh0`) which scalar property iteration doesn't
  see. Lyra Sage Gap #13 follow-up.
- CDO is now retrieved with `bCreateIfNeeded=true` so cold-loaded native
  classes don't return empty.

### bp.add_node (schema location TBD by schema-owning agent)
- Behaviour: when the target graph's schema is one of the AnimGraph
  schemas (`AnimationGraphSchema`, `AnimationStateGraphSchema`,
  `AnimationStateMachineSchema`, `AnimationTransitionSchema`), the
  handler now rejects -32602 with
  "use animation.add_animgraph_node for AnimGraph nodes (canonical
  Cluster A path)".
- Description bullet: "Reject AnimGraph contexts; use
  `animation.add_animgraph_node` instead."

### bp.validate (schema location TBD)
- Behaviour clarification: handler now snapshots the package dirty bit
  pre-compile and restores it after, so a read-only validation no longer
  flips the asset to dirty just because `EBlueprintCompileOptions::SkipSave`
  still mutates the skeleton.
- No schema field change.

### bp.run_construction_script (schema location TBD)
- Behaviour clarification: temporary spawned actor is now destroyed via
  `ON_SCOPE_EXIT` so the throwaway actor cannot leak on early-return
  paths. No public surface change.

### bp.reparent_component (schema location TBD)
- Behaviour clarification: handler now pre-validates that BOTH the source
  component and `new_parent` are `USceneComponent` descendants BEFORE any
  SCS detachment, so a validation failure can no longer leave the SCS
  half-detached. Two new error messages added:
  - "component is not a USceneComponent; SCS reparent only valid for scene components"
  - "new_parent is not a USceneComponent; cannot host scene-component children"
- Description bullet: "Both component and new_parent must be
  USceneComponent descendants."

### Param-match audit (handler reads vs schema names)
Walked every `bp.*` handler in `SageBlueprintTools.cpp`. Handler param
names that match what's checked into the file align with the schemas
inspected in `server/src/main.cpp` and
`server/src/tools/phase4_schemas.cpp`:
- `path`, `name`, `type`, `type_object`, `is_array`, `default_value`,
  `confirmed`, `function`, `node_id`, `node_class`, `node_x`, `node_y`,
  `node_params`, `pin`, `value`, `from_node`, `from_pin`, `to_node`,
  `to_pin`, `component`, `new_parent`, `property`, `class`, `properties`,
  `interface_path`, `preserve_functions`, `direction`, `old_name`,
  `new_name`, `new_parent_class`, `new_class`, `recompile`, `output_path`,
  `include_function_graphs`, `include_t3d`, `include_referenced_assets`,
  `include_component_defaults`, `include_all_nodes`, `t3d`, `pos_x`,
  `pos_y`, `node_ids`, `dry_run`, `exclude`, `except`, `scope`,
  `keep_entry_nodes`, `keep_event_entries`, `instance_editable`,
  `replicated`, `expose_on_spawn`, `category`, `tooltip`, `parent_class`,
  `keyword`, `filter`, `max`, `access`, `fn_name`, `source`, `destination`,
  `tick_interval`, `tick_group`, `can_tick`, `reverse`.
- Two handler-only param adds this round: `confirmed` on
  `bp.delete_variable` + `bp.delete_function` (schema entries listed
  above).
- No other drift detected.

### GameThread marshal audit
- All 59 `Dispatch.RegisterHandler(...)` calls in
  `RegisterBlueprintTools` (line ~4832) wrap their impl with the local
  `GT(&BpXxxImpl)` wrapper; no bare registrations.

---

## Agent 5/8 — SageProjectTools.cpp

### project.set_plugin_enabled
- Add required input: `confirmed: boolean` (default false). Tool now rejects calls without `confirmed=true` since modifying `.uproject` Plugins[] during a running editor session has non-trivial side effects (modules unload, refs dangle, restart required).
- Add output field: `requires_editor_restart: boolean` (always true after a successful mutation). Replaces the human-only `note` string for programmatic detection.
- Removed output field: `backup` (the `.sage_bak` file is now deleted on successful AtomicWriteString — backup was previously a string pointing to a non-existent file).

### project.set_config
- Removed output field: `backup`. Same reason as above (backup deleted on success).

### project.write_cpp_file
- Add required input: `confirmed: boolean` (default false). Arbitrary source overwrite is high-risk and now gated.
- Add input semantics note: path must resolve under `FPaths::ProjectDir()` (engine source writes rejected) and extension must be one of `.h | .cpp | .inl | .cs | .Build.cs | .Target.cs`. INI/`.uproject`/`.uplugin` are explicitly rejected — agents should use the dedicated tools (`project.set_config`, `project.set_plugin_enabled`).

### project.create_cpp_class
- Add output field: `requires_editor_restart: boolean` (true only when `bootstrapped=true`; false otherwise — class added to existing module can be picked up via Live Coding on Windows).

### project.add_module_dependency
- No schema change (input/output unchanged). Behavioural fix: Lyra Gap #9 regression — substring-only pre-existence check now does exact-quoted entry match against the parsed AddRange block, so adding `Core` to a module that already has `CoreUObject` no longer reports a false `already_present`.

### project.set_project
- Add output field: `skipped_keys: string[]` (optional). Lists keys whose JSON values had unsupported types (object/null) and were skipped instead of silently coerced via `AsString()`.
- Behavioural change (no schema change): boolean / number / array values are now serialized via `GConfig->SetBool` / `SetDouble` / `SetArray` instead of `SetString` on `AsString()` output. Booleans now emit as `True`/`False` and round-trip through UE's INI writer cleanly.

---

## Agent 7/8 — SageWidgetTools.cpp / SageSequencerTools.cpp

### widget.anim.add_track
- Behavior change documented: handler now rejects with -32602 when `binding_guid` does not correspond to a possessable on the target animation (i.e. caller skipped `widget.anim.bind`). Schema description should note precondition: "binding_guid must be returned from a prior widget.anim.bind call". No schema property change.

### widget.create_utility_widget
- Behavior change: handler now uses `UEditorUtilityWidgetBlueprintFactory` (was `UWidgetBlueprintFactory`). Result asset is now a `UEditorUtilityWidgetBlueprint` (not plain `UWidgetBlueprint`) — the form needed for the Editor Utility menu and `widget.run_utility_widget`.
- New response field documented: `class: string` — concrete blueprint class created (typically `EditorUtilityWidgetBlueprint`). Schema description should mention "asset is a UEditorUtilityWidgetBlueprint".

### widget.run_utility_widget
- No longer a stub. Handler now calls `UEditorUtilitySubsystem::SpawnAndRegisterTabAndGetID` and returns the spawned widget + tab id.
- New response fields documented:
  - `tab_id: string` — registered nomad tab id
  - `widget: string` — spawned `UEditorUtilityWidget` object name
  - `class: string` — runtime class name
  - `spawned: bool` — true on success
- Schema description should drop any "[STUB]" prefix and read: "Spawns the editor utility widget as a registered nomad tab via UEditorUtilitySubsystem. Requires the asset to be a UEditorUtilityWidgetBlueprint (created by widget.create_utility_widget)."

### widget.anim.bind
- No schema change. Internal docs verified: `WidgetName` resolution at runtime is `WidgetTree.FindWidget(*WidgetName.ToString())` matching `UWidget::GetName()`, so `Target->GetName()` is correct (BP-generated widget object name == property/variable name). Verified against UE 5.7 `Runtime/UMG/Private/Animation/WidgetAnimationBinding.cpp`.

### Param-match audit (SageWidgetTools.cpp)
- All `widget.*` handlers marshalled via the `GT(...)` registration wrapper (`RegisterWidgetTools` lambda calling `detail::RunOnGameThread`) — GameThread-safe. No bare registrations.
- No param-name drift detected between handler reads and registered tool names.

### seq.add_keyframe
- Major contract change — handler is no longer a stub.
- Add required params:
  - `track_name: string` — UMovieSceneTrack object name to target (use the `name` field from `seq.list_tracks`)
  - `time: number` — seconds (already in schema)
  - `value: number | bool` — numeric for float/double/integer channels, boolean for bool channels
- Add optional param:
  - `channel_index: integer` (default 0) — channel index within the section's channel proxy when the track exposes multiple homogeneous channels
- New response fields:
  - `track_name: string`
  - `frame: integer` — converted FFrameNumber (tick resolution)
  - `channel_index: integer`
  - `channel_type: string` — one of `"float" | "double" | "integer" | "bool"`
- Updated required list: `["path", "track_name", "time", "value"]`
- Description: "Add a keyframe to the first section of a named track. Auto-creates the section if absent. Resolves channel by type fallback: float → double → integer → bool at the given index."

### seq.add_spawnable
- New response field documented: `guid: string` — the FGuid (digits-with-hyphens) returned from `MovieScene::AddSpawnable`. Use this to subsequently target the binding.
- Behaviour change (no schema impact): template object now created with `RF_Transactional | RF_ArchetypeObject` (was just `RF_Transactional`) — required by UE 5.7 spawnable serialize/spawn path.

### seq.list_tracks
- No schema change. Internal: switched non-const `MS->GetBindings()` to const-pointer dispatch to silence UE 5.7 deprecation warning.

### Param-match audit (SageSequencerTools.cpp)
- All `seq.*` handlers marshalled via the `GT(...)` registration wrapper (`RegisterSequencerTools`) — GameThread-safe. No bare registrations.
- `seq.add_keyframe` formerly accepted only `path`+`time` (stub); see new contract above.
- No other param-name drift detected.

### Agent 7/8 — Re-audit (2026-05-04) — param-name drift between schema and handler reads

The following are concrete schema/handler param-name mismatches found while re-auditing both files. Handler reads (canonical) listed; schema param names need to match.

#### widget.move_widget
- Schema currently declares `{"path", "widget", "new_parent"}` required.
- Handler reads `path`, `widget_name`, `new_parent` (`SageWidgetTools.cpp:~730`).
- Fix: rename schema param `widget` → `widget_name`. Required list: `["path","widget_name","new_parent"]`.

#### seq.add_keyframe
- Schema currently declares `{"path","track","time","value"}` required.
- Handler reads `path`, `track_name`, `time`, `value` (`SageSequencerTools.cpp:~213-225`).
- Fix: rename schema param `track` → `track_name`. (Already noted above that `track_name` is the canonical contract.)

#### seq.add_possessable
- Schema currently declares `{"path","actor"}` required.
- Handler reads `path`, `actor_id` (`SageSequencerTools.cpp:~366-369`).
- Fix: rename schema param `actor` → `actor_id`.

#### seq.add_spawnable
- Schema currently declares `{"path","class","name"}` with required `["path","class"]`.
- Handler reads `path`, `class_path` (`SageSequencerTools.cpp:~407-410`); `name` is unused in handler.
- Fix: rename schema param `class` → `class_path`. Drop unused `name` (or note as accepted-but-ignored). Required list: `["path","class_path"]`.

---

## Agent 8/8 — SageEditorTools.cpp / SageEditorAutomationTools.cpp

### editor.console_command
- Behavioural tightening (no schema property change): whitelist now matches
  exact-or-prefix-with-space (e.g. `STAT` matches `STAT FPS` but not
  `STATQUIT`; `OBJ` matches `OBJ LIST` but not `OBJDUMP`). Previously a
  pure prefix match permitted bypasses.
- IO-redirecting forms (`FILE=`, `-FILE=`, `OUTPUTLOG`, ` > `, ` >> `, ` | `,
  leading `EXEC `) are now rejected -32602 even before the whitelist check;
  they require explicit `allow_unsafe=true` to proceed.
- Description should mention: "IO-redirecting console forms (FILE=, pipes,
  EXEC) are rejected unless allow_unsafe=true."

### editor.take_screenshot
- Behavioural tightening: `path` is now sandboxed to
  `<Project>/Saved/Screenshots`. Absolute paths outside that root and `..`
  segments that would escape it are rejected -32602 ("screenshot path must
  be under '<root>'"). Path comparison is case-insensitive on Windows,
  case-sensitive elsewhere.
- Description should note: "path must resolve under
  <Project>/Saved/Screenshots; absolute paths and `..` escapes outside
  that directory are rejected."

### editor.run_python
- Response now includes `_security_warning: string` on every successful
  call: "executes arbitrary Python with full UE access; restrict before
  public release". Phase 5+ auth-gate territory — kept enabled but
  self-describing.
- Description should mention: "Returns `_security_warning`; this tool
  exposes arbitrary Python with full UE editor access and is expected to
  be auth-gated server-side before public release."

### editor.build_all
- Add required input: `confirmed: boolean` (default false). Long-running
  destructive op (lighting can take hours; consumes editor). Rejects
  -32602 without `confirmed:true`.
- Updated required list: `["confirmed"]`.

### editor.build_geometry
- Add required input: `confirmed: boolean` (default false). Same gate as
  `editor.build_all`.
- Updated required list: `["confirmed"]`.

### editor.build_lighting
- Add required input: `confirmed: boolean` (default false). Same gate.
- Updated required list (with existing `quality` optional): `["confirmed"]`.

### editor.build_hlod
- Add required input: `confirmed: boolean` (default false). Same gate.
- Updated required list: `["confirmed"]`.

### editor.get_crash_info
- Add optional input: `lines: integer` (default 50, min 1, max 10000) —
  size of crash log tail to return.
- Add optional input: `offset_from_end: integer` (default 0, min 0) —
  number of lines to skip from the end before applying `lines` window.
- Response now echoes both `lines` and `offset_from_end`. Crash log
  filename is now `<ProjectName>.log` (composed from `FApp::GetProjectName()`)
  with `UnrealEditor.log` retained as fallback — the legacy hardcoded
  `SageTest.log` is gone.
- Description bullet: "Optional `lines` (default 50, max 10000) and
  `offset_from_end` (default 0) control the log tail window."

### editor.list_crashes
- No schema property change. Behavioural fix: `has_log` now checks
  `<ProjectName>.log` (dynamic via `FApp::GetProjectName()`) and
  `UnrealEditor.log` instead of the legacy hardcoded `SageTest.log`.

### save_level
- No schema change. Behavioural fix: `UEditorLoadingAndSavingUtils::SavePackages`
  now passes `bOnlyDirty=true` so calling `save_level` on an unmodified
  world is a no-op (avoids gratuitous mtime bumps and VCS noise). Response
  still surfaces `was_dirty` for caller awareness.

### Param-match audit (SageEditorTools + SageEditorAutomationTools)
- Walked every handler in both files. Param names match the registered
  schema names — no drift detected. Audited inputs:
  - SageEditorTools: `actor_ids` (select_actors).
  - SageEditorAutomationTools: `cmd`, `allow_unsafe`, `path`, `filter`,
    `max_lines`, `code`, `quality`, `query`, `max_results`, `within_hours`,
    `crash_dir`, `lines`, `offset_from_end`, `property`, `value`, `factor`,
    `actor_id`, `active_viewport_only`, `location`, `rotation`, `group`,
    `level`, `output_path`, `size`, `directory`, `platform`, `category`,
    `confirmed`.
- `confirmed` is a new required input on `editor.build_all`,
  `editor.build_geometry`, `editor.build_lighting`, `editor.build_hlod`
  (schema entries listed above).

### GameThread marshal audit
- `SageEditorAutomationTools.cpp`: every handler in
  `RegisterEditorAutomationTools` (line ~1083) is wrapped with the local
  `GT(...)` lambda → `detail::RunOnGameThread`. No bare registrations.
- `SageEditorTools.cpp`: each registered handler
  (`GetWorldHandler`/`SaveLevelHandler`/etc.) explicitly calls
  `detail::RunOnGameThread([Args]() { ... })` inside its body. All 10
  registrations covered.
- No drift detected.

---

## fix-agent 6 — mat.* audit (2026-05-04)

Files audited: `plugin/Source/SageBridge/Private/Tools/SageMaterialGraphTools.cpp`,
`plugin/Source/SageBridge/Private/Tools/SageMaterialTools.cpp`.

**Status of P0 fixes:** all 11 already applied in current source — no .cpp edits
needed this pass.
- #1 `mat.disconnect`: NOT a silent no-op. Real impl walks
  `FExpressionInputIterator` for per-expression case and clears the named
  `FExpressionInput` on `UMaterialEditorOnlyData` for the property case.
  `UMaterialEditingLibrary::DisconnectExpression` does NOT exist in 5.7
  (verified `MaterialEditingLibrary.h:162` only declares
  `ConnectMaterialExpressions`). Direct field-clear is correct.
- #2 Per-material transactions: `MatTransactions()` is
  `TMap<TWeakObjectPtr<UMaterial>, TUniquePtr<FScopedTransaction>>` keyed
  by Material*, dead-weak GC pass on each begin. ADR-017 safe.
- #3 `mat.set_expression_value`: handles Constant, Constant3Vector,
  Constant4Vector, VectorParameter, ScalarParameter, ConstantBiasScale.
- #4 `mat.set_base_color` / `mat.set_blend_mode` / `mat.set_shading_model`:
  full Modify discipline (`PreEditChange(nullptr)` + `EOD->Modify()` +
  `M->Modify()` + `PostEditChange()`).
- #5 `_compile_warning` field present on every recompile-triggering response.
- #6 `MSM_Strata` enum still exists in 5.7 (`EngineTypes.h:718` —
  `UMETA(DisplayName="Substrate", Hidden)`); display string toggled
  via `ENGINE_MINOR_VERSION >= 7` guard. Correct.
- #8 `RegisterMaterialGraphTools` wraps every handler in `GT(...)` lambda
  → `detail::RunOnGameThread`. `RegisterMaterialTools` uses per-handler
  `RunOnGameThread`. Both safe.
- #9 `mat.connect_expressions` uses canonical
  `UMaterialEditingLibrary::ConnectMaterialExpressions`.
- #10 `mat.create` factory null check present.
- #11 `mat.validate` returns `_status: "queued"` + `_compile_warning`.

### mat.duplicate
- Schema (`phase4_schemas.cpp:1507-1510`) requires field `dest`.
- Handler (`SageMaterialGraphTools.cpp:1037-1062`) reads
  `TryGetStringField(TEXT("destination"))`.
- **MISMATCH** — caller passing schema-correct `dest` will hit
  "missing 'destination'". Pick one: rename schema → `destination`
  (matches every other mat.* tool that uses "destination", e.g.
  `mat.create_instance`), or accept both in handler.
- Recommendation: rename schema field `dest` → `destination` for
  consistency with `mat.create_instance`.

### mat.import_graph
- Schema (`phase4_schemas.cpp:1522-1525`) requires object field `graph`.
- Handler (`SageMaterialGraphTools.cpp:1140`) reads
  `TryGetArrayField(TEXT("nodes"))`.
- **MISMATCH** — schema-driven caller will fail. Either:
  (a) schema → `{path, nodes:array}` to match handler, or
  (b) handler → unwrap `args["graph"]["nodes"]`.
- Recommendation: (a). The `mat.export_graph` companion already returns
  top-level `nodes` array, so symmetric round-trip wants `nodes` at top
  level on import too.

### mat.build_graph
- Schema (`phase4_schemas.cpp:1527-1530`) requires object field `spec`.
- Handler (`SageMaterialGraphTools.cpp:1188-1217`) reads `base_color`,
  `metallic`, `roughness`, `emissive` directly from top-level args.
- **MISMATCH** — caller wrapping in `spec` will get a no-op build (all
  `TryGetArrayField` calls fail silently, returns
  `{material, built:true}` with zero nodes added).
- Recommendation: schema → `obj({{"path",str()},{"base_color",arr()},
  {"metallic",num()},{"roughness",num()},{"emissive",arr()}}, {"path"})`
  to match handler. Drop the opaque `spec` wrapper; flat shape matches
  `MatBuildGraphImpl` and is more discoverable in `tools/list`.

### mat.begin_transaction
- Schema (`phase4_schemas.cpp:1537-1540`) has `label`.
- Handler (`SageMaterialGraphTools.cpp:1272-1306`) reads `description`
  and `path` (per-material keying).
- **MISMATCH** — `label` is unused; `path` (critical for ADR-017
  per-material scoping) missing from schema entirely.
- Recommendation: schema →
  `obj({{"path",str()},{"description",str()}})`.
  Without `path` exposed, callers can't safely use the per-material map
  and will collide on the null-key "global" slot, defeating fix #2.

### mat.end_transaction
- Schema (`phase4_schemas.cpp:1542-1545`) is empty.
- Handler (`SageMaterialGraphTools.cpp:1311`) reads optional `path`.
- **MISMATCH (optional)** — caller can't pass `path` per schema → forced
  into global-key commit, again defeating per-material scoping.
- Recommendation: schema → `obj({{"path",str()}})`.

### mat.list_expression_types
- Schema has `filter` (string). Handler ignores all args.
- **Cosmetic** — either implement filter (substring match on
  `Cls->GetName()`) or drop from schema. Low priority.

### mat.render_preview
- Schema has `resolution` (int). Handler ignores.
- **Cosmetic** — handler is itself a stub returning thumbnail dir; drop
  `resolution` from schema until real bake is implemented.

### mat.connect_texture (cross-check)
- Handler also accepts `UMaterialExpressionTextureSampleParameter` via
  `Cast<UMaterialExpressionTextureBase>` (TextureBase is the common
  ancestor). Schema correctly says "TextureBase". OK.

### Skipped (no schema-fixes pass needed)
- `mat.read`, `mat.list_parameters`, `mat.list_expressions`,
  `mat.create_instance`, `mat.add_expression`, `mat.delete_expression`,
  `mat.connect_expressions`, `mat.connect_to_property`,
  `mat.set_expression_value`, `mat.connect_texture`,
  `mat.set_shading_model`, `mat.set_base_color`, `mat.validate`,
  `mat.create`, `mat.set_blend_mode`, `mat.disconnect`, `mat.recompile`,
  `mat.get_shader_stats`, `mat.export_graph`, `modify_material_parameter`
  — schemas in `server/src/main.cpp` and `phase4_schemas.cpp` lines
  cited above match handler `TryGet*Field` calls.

---

## Agent 3/8 — SageAssetTools.cpp / SageAssetAdvancedTools.cpp

### delete_asset (server/src/main.cpp:~500)
- Add property: `confirmed: bool` (required).
- Updated required list: `["asset_path","confirmed"]`.
- Description: append "Pass `confirmed:true` to proceed (destructive)."

### asset.delete_batch (server/src/main.cpp:~3030)
- Add property: `confirmed: bool` (required).
- Updated required list: `["paths","confirmed"]`.
- Description: append "Pass `confirmed:true` to proceed (destructive — deletes every listed asset; only undoable while editor is alive)."

### asset.reload_package (server/src/main.cpp:~3052)
- Add property: `confirmed: bool` (required).
- Updated required list: `["path","confirmed"]`.
- Description: append "Pass `confirmed:true` to proceed (destructive — discards in-memory edits, irreversible without git)."

### asset.fixup_redirectors (server/src/main.cpp:~2318)
- Add property: `confirmed: bool` (required).
- Updated required list: `["confirmed"]`.
- Description: append "Pass `confirmed:true` to proceed (destructive — mid-refactor redirect chains will be lost; FixupReferencers rewrites referencing assets and deletes the redirectors)."

### asset.add_array_element (server/src/tools/phase4_schemas.cpp:~1816)
- Add optional property: `allow_empty: bool` (default false). When true, an instanced subobject branch tolerates zero successfully-applied fields instead of rolling back the array append.
- Document new optional response field: `skipped_fields: string[]` — names of subobject UPROPERTY fields whose JSON values failed `SetUPropertyFromJson` (visible to caller so failed fields aren't silent).

### asset.read_datatable (server/src/main.cpp:~2879)
- Add optional properties (mirror `asset.list` Lyra Gap #2 pagination):
  - `offset: integer` (minimum 0, default 0) — skip first N rows.
  - `fields: string[]` — projection filter; if set, each row's `fields` object only contains listed columns.
- Update response shape note: `{rows, returned, offset, capped, ...}` (offset now echoed; capped already present).

### asset.import_animation (server/src/main.cpp:~2792)
- Add required property: `skeleton: string` — UAnimSequence imports REQUIRE a target USkeleton path. Without it, UFbxImportUI refuses to bind the AnimSequence and the resulting asset is unusable. Handler now validates and rejects -32602 if missing or wrong class.
- Updated required list: `["file","destination","skeleton"]`.
- Description: replace with "Import a UAnimSequence from FBX. Verifies the produced asset is a UAnimSequence. Requires `skeleton` (USkeleton path) — wired into UFbxImportUI->Skeleton + bImportAnimations=true. -32602 if skeleton missing/wrong class."

### asset.set_mesh_nav (server/src/tools/phase4_schemas.cpp:~2032)
- Document new optional response field: `skipped_fields: string[]` — populated when reflection-based property write fails (silent-fail anti-pattern fix).

### asset.recenter_pivot (server/src/tools/phase4_schemas.cpp:~2017)
- Description: clarify "Returns `modified=false` with a `note` field — true pivot recentering requires vertex offset baking which is not implemented. Treat this as a placeholder; use the editor's `right-click in viewport > Pivot > Set as Pivot Offset` workflow or re-import the FBX with adjusted origin."

### Param-match audit (SageAssetTools.cpp + SageAssetAdvancedTools.cpp vs schema)
Walked every `Args->TryGet*Field(TEXT("..."))` call against the matching schemas in `server/src/main.cpp:~2239+` (cluster 4.5) and `server/src/tools/phase4_schemas.cpp:~1816 / ~2017+`. Drift findings:
- `asset.import_animation`: handler reads `skeleton`, schema does not declare → MISMATCH (fix listed above).
- `asset.delete_batch` / `asset.reload_package` / `asset.fixup_redirectors` / `delete_asset`: handler reads `confirmed`, schemas do not declare → MISMATCH (fixes listed above).
- `asset.read_datatable`: handler reads `offset` + `fields`, schema does not declare → MISMATCH (fix listed above).
- `asset.add_array_element`: handler reads `allow_empty`, schema does not declare → MISMATCH (fix listed above).
- All other handlers checked (modify_asset_property, rename_asset, move_asset, duplicate_asset, save_assets, get_dirty_assets, discard_changes, asset.get_mesh_bounds, asset.get_mesh_collision, asset.bulk_rename, asset.move_folder, asset.list_redirectors, asset.diagnose_registry, asset.list, asset.search, asset.read_properties, asset.list_sockets, asset.add_socket, asset.remove_socket, asset.list_textures, asset.get_texture_info, asset.set_texture_settings, asset.create_data_asset, asset.list_mesh_materials, asset.set_mesh_material, asset.set_sk_material_slots, asset.create_datatable, asset.reimport_datatable, asset.import_texture / static_mesh / skeletal_mesh, asset.reimport, asset.export, asset.recenter_pivot, asset.set_mesh_nav, asset.search_fts, asset.reindex_fts) — no other param-name drift detected.

### GameThread marshal audit (SageAssetTools.cpp + SageAssetAdvancedTools.cpp)
- `SageAssetTools.cpp`: every handler in `RegisterAssetTools` is the explicit `XxxHandler` shim that wraps `XxxOnGameThread` via `detail::RunOnGameThread([Args]() { ... })` — 8 handlers, all covered (`ModifyAssetProperty`, `RenameAsset`, `MoveAsset`, `DuplicateAsset`, `DeleteAsset`, `SaveAssets`, `GetDirtyAssets`, `DiscardChanges`).
- `SageAssetAdvancedTools.cpp`: `RegisterAssetAdvancedTools` defines a local `GT(&Impl)` lambda that wraps every `*Impl` in `detail::RunOnGameThread([&]() { ... })`. Every `Dispatch.RegisterHandler(...)` call goes through `GT(...)` — 36 handlers, no bare registrations.
- No drift detected.
