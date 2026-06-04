# Sage Gap Inbox

This file is the working inbox for Sage MCP tool gaps found while dogfooding Lyra, Kale, HeroFlight, and related Unreal projects.

Path:

```text
D:\Steamworks\sage-unreal-mcp\.claude\notes\gap-inbox.md
```

## Workflow

- Add new gaps under `Open Detailed Gaps`, newest first.
- Keep open gaps detailed enough for Sage implementation: blocked task, repro/observed error, expected behavior, acceptance criteria, and cleanup/workaround.
- When a gap is implemented, move it to `Implemented / Closed Summary` as a single-line summary.
- Only keep detailed text for gaps that are still open or blocked.

## Status Legend

- OPEN: not implemented or still blocking project work.
- IN-PROGRESS: Sage implementation is underway.
- FIXED: source/tool implementation exists, but deploy or in-engine verification may still be pending.
- CLOSED: verified by a dogfooding project.
- WONTFIX: intentionally not implemented.
- DUPLICATE: covered by another entry.

## Implemented / Closed Summary

- Sage Gap 2026-06-03 Niagara gap sweep: FIXED in source; expanded Niagara from 26 advertised tools to 68 schema/handler-aligned tools and replaced successful placeholder notes with either real read/write/readback behavior or hard unsupported MCP errors. Implemented source-level coverage for preview spawn/cleanup, component parameter writes/readback, system/emitter creation, emitter handles, shared-emitter localization guard, module graph read/export/basic mutation, module input/static switch writes, renderer CRUD/property writes, system user parameters, Niagara Parameter Collections, bounds/warmup/effect type/scalability, validation, SimCache metadata/capture surface, event/simulation-stage listing and removals, dependency/referrer audits, and batch execution. Added Niagara runtime/editor module dependencies, `.uplugin` Niagara dependency, registry canary tests, and `.claude/notes/niagara-mcp-reference-matrix.md`. Build/package/deploy/live Kale dogfood are intentionally pending per user instruction on 2026-06-03/2026-06-04.
- Sage Gap 2026-06-02 IKRig skeletal mesh setter for existing rig: FIXED in source; added `animation.set_ik_rig_skeletal_mesh` to set an existing/duplicated `UIKRigDefinition` preview skeletal mesh via `UIKRigController`, with `skeletal_mesh`/`skeletal_mesh_path`/`mesh` aliases, `dry_run`/`validate_only`, `save`, compatibility diagnostics from UE's IKRig processor, before/after `animation.read_ik_rig` readback, and preservation flags for retarget root, chains, goals, solvers, and goal-solver connection counts. Incompatible meshes are rejected without dirtying. `animation.read_ik_rig` now also reports goal-solver connections. Build, package, deploy, and live Kale verification are intentionally pending per user instruction on 2026-06-02.
- Sage Gap 2026-06-02 Blueprint reparent inherited native CDO refresh: FIXED in source; `bp.reparent` now compiles after parent-class changes so the generated class layout exposes inherited native CDO properties immediately, and `bp.set_cdo_property` now supports `dry_run`/`validate_only`, stale generated-class refresh for non-dry inherited parent properties, before/after/readback fields, and `owner_class`/generated/parent class diagnostics. Build, package, deploy, and live Kale verification are intentionally pending per user instruction on 2026-06-02.
- Sage Gap 2026-06-02 IK Retargeter IK chain settings writer: FIXED in source; added `animation.set_ik_retargeter_ik_chain_settings` to edit one existing `IKRetargetIKChainsController` target-chain settings row by op name/index or first IK Chains op, including `static_offset`, `static_local_offset`, `static_rotation_offset`, `blend_to_source*`, `blend_to_source_weights`, `apply_pelvis_offset_to_source_goals`, `scale_vertical`, `extension`, dry-run/validate-only, before/after settings, save, and full `animation.read_ik_retargeter` readback. Missing target chains are rejected before mutation. Build, package, deploy, and live Kale verification are intentionally pending per user instruction on 2026-06-02.
- Sage Gap 2026-05-31 Mesh-only socket override when skeleton socket already exists: FIXED in source, packaged, and deployed to `D:\GameDev\Kale`; added `asset.get_socket`, `asset.upsert_socket`, and `asset.update_socket` alias, extended `asset.list_sockets` and `animation.list_sockets` with owner filters (`mesh`/`skeleton`/`any`), effective ownership readback, mesh-vs-skeleton collision diagnostics, and mesh-only override support when `allow_mesh_override_of_skeleton_socket:true`. Skeletal mesh writes scoped to `owner:"mesh"` create/update only the `USkeletalMesh` socket and report `mutated_skeleton:false`; shared skeleton writes require explicit `owner:"skeleton"` plus `confirmed:true`. Verified by debug `sage-server` build, 37/37 ctest, tool audit, UE BuildPlugin, and matching Kale DLL SHA256.
- Sage Gap 2026-05-30 Control Rig real authoring/readback surface: FIXED in source, packaged, and deployed to `D:\GameDev\Kale`; `animation.list_control_rig_variables` is now a real Control Rig readback compatibility alias and new `controlrig.read`, `controlrig.list_controls`, `controlrig.set_preview_mesh`, `controlrig.set_control_transform`, `controlrig.add_control`, and `controlrig.remove_control` tools resolve `UControlRigBlueprint`, inspect `URigHierarchy`, expose controls/bones/nulls/curves/connectors/sockets/root elements, mutate preview mesh/control transforms/hierarchy through editor/controller APIs, and support dry-run/compile/save/readback. Build.cs/uplugin now include ControlRig/ControlRigDeveloper/ControlRigEditor dependencies. Verified by debug `sage-server` build, 37/37 ctest, tool audit, UE BuildPlugin, and matching Kale DLL SHA256.
- Kale Gap 2026-05-21 WidgetBlueprint CDO instanced UObject array authoring: FIXED in source and packaged; added `bp.set_cdo_instanced_array_element` to upsert `EditAnywhere, Instanced` UObject array entries on Blueprint/WidgetBlueprint generated-class CDOs. The tool resolves Blueprint asset paths to CDOs, validates instanced UObject array shape and subclass compatibility, creates or updates the subobject idempotently by `match_fields`, or by `Presentation` + `WidgetClass` for reticle-provider style data, applies reflected property values including enums and class refs, supports `dry_run`/`validate_only`/`compile`/`save`, returns recursive readback, and surfaces typed applied/failed/unknown field errors. `bp.get_cdo_properties` now also accepts Blueprint asset paths and supports `recurse_instanced`/`max_depth` CDO readback for provider arrays. Verified by debug `sage-server` build, rebuilt `sage-tests` + 37/37 ctest, tool audit, and UE BuildPlugin.
- Kale Gap 2026-05-21 GameFeature AddWidgets HUD entries: FIXED in source and packaged; added `gamefeature.add_widget_entry` to author Lyra `UGameFeatureAction_AddWidgets` HUD extension entries via runtime reflection without a hard LyraGame dependency. The tool validates `UUserWidget`/CommonActivatableWidget class paths and existing GameplayTags, creates the AddWidgets action when requested, writes `Widgets` and optional `Layout` entries idempotently by tag+class, supports `dry_run`/`validate_only`/`save`, preserves existing actions and entries, and returns full readback. Verified by debug `sage-server` build, rebuilt `sage-tests` + 37/37 ctest, tool audit, and UE BuildPlugin.
- Kale Gap 2026-05-18 LayeredBoneBlend BlendMask arrays: FIXED in source and packaged; added `animation.set_layered_bone_blend_config` for atomic `FAnimNode_LayeredBoneBlend` config edits, including `BlendMask` mode, `UBlendProfile` subobject resolution from `SkeletalMesh.SkeletalMesh:MaskName` or class-qualified paths, BlendMask validation, `BlendWeights` length checks, BranchFilter `LayerSetup` restoration, mesh/root/scale blend flags, curve blend option, compile/save, dry-run validation, and before/after readback. Failed profile or length validation now returns before mutation, avoiding the partial `BlendMode` state from the repro. Verified by debug `sage-server` build, rebuilt `sage-tests` + 37/37 ctest, tool audit, and UE BuildPlugin.
- Kale Gap 2026-05-17 AnimBP state bound graph inspection: FIXED in source and packaged; added `animation.read_state_graph` for state-machine state bound graphs with graph identity, outer chain, node kind/title/coordinates, optional pins/links, reflected `FAnimNode_*` properties, animation asset references, property-access bindings, output pose source, ambiguity handling, and node/class/asset filters. `animation.read_anim_graph` now targets root/layer/state/transition graphs by `graph_name`, `animation.read_anim_node_properties` and `animation.list_animgraph_nodes` expose animation asset readback, and `bp.read_function_graph` returns an anim-graph hint instead of only `function graph not found` for AnimBP state graphs. Verified by debug server schema tests and UE BuildPlugin.
- Kale Gap 2026-05-15 animation.remove_animgraph_node state-machine graph ownership corruption: FIXED in source and packaged; state-machine nodes now report `EditorStateMachineGraph` path/name, outer chain, owner/anchor flags, reference count, and safe-delete status. `animation.remove_animgraph_node` rejects owning/anchor `UAnimGraphNode_StateMachineBase` removal, safely detaches non-owning duplicate references before `RemoveNode`, and new `animation.remove_state_machine_reference_node` provides dry-run diagnostics for this cleanup. `animation.add_state_machine_node` marks Sage-authored reference nodes and returns ownership readback. Verified by debug server schema tests and UE BuildPlugin.
- Kale Gap 2026-05-13 animation.set_owner_locomotion_update pin connection crash: CLOSED/VERIFIED in Kale dogfood; FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; owner-locomotion value pins are no longer held through a mutable `TMap` pointer after reallocation, vector/bool/dot helpers now validate missing source pins before creating or connecting K2 nodes, and pin resolution failures return MCP errors instead of crashing the editor. Verified by UE BuildPlugin, deployed DLL hash match, and a clean Kale repro where `animation.set_owner_locomotion_update` authored all nine owner-locomotion assignments with `compiled:true` and no editor disconnect.
- Kale Gap 2026-05-13 animation.set_owner_locomotion_update variable-set crash: FIXED in source and packaged; `UK2Node_VariableSet` assignment pins are now resolved through input-pin lookup instead of `GetValuePin()`, owner-locomotion K2 connections now validate output-to-input direction before schema connection, and `bp.add_variable` marks Blueprints structurally modified/dirty so same-bulk variable creation feeds later K2 node pin generation. Verified by UE BuildPlugin; deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`.
- Kale Gap 2026-05-13 AnimBP flight locomotion property-access authoring: FIXED in source; added `animation.set_owner_locomotion_update` to author typed, Sage-marked `BlueprintUpdateAnimation` K2 owner/CharacterMovement update chains with owner/CMC casts, movement intent or custom-mode checks, velocity/speed/input projections, variable assignment readback, schema exposure, and overwrite guards. Verified by UE BuildPlugin, release server build, and release ctest.
- Kale Gap 2026-05-12 DataAsset TSubclassOf property write for LyraPawnData: FIXED in source; `SetUPropertyFromJson` now resolves quoted Blueprint generated class paths, Blueprint asset paths, and package paths for `FClassProperty` / `TSubclassOf<T>`, validates against the property's `MetaClass`, avoids nulling the property on invalid class paths, and `modify_asset_property` / `editor.set_property` now return class readback via `resolved_class`, `meta_class`, and `readback`. Verified by UE BuildPlugin, release server build, and release ctest.
- Kale Gap 2026-05-12 safe native PIE actor spawn and readback: FIXED in source; added `gameplay.spawn_pie_actor_snapshot` for transient PIE actor spawn, actor/component/property readback, forced destroy-before-return, optional replication flags, target PIE world selection, and Python/UE reference cleanup. Verified by UE BuildPlugin and release server build.
- Kale Gap 2026-05-11 PIE keyboard input does not reach Lyra Enhanced Input + GAS pipeline: FIXED in source; added `gameplay.simulate_input_tag`, resolving Lyra-style `InputAction` bindings from input tags, executing Enhanced Input bound delegates, and calling `PlayerController::PostProcessInput` so Lyra ASC input processing runs without a hard Lyra dependency. Verified by UE BuildPlugin and release server build.
- Kale Gap 2026-05-11 asset.migrate_from_project write-set guard rejects selected animation packages: CLOSED/VERIFIED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; `actual_write_side_effects` now treats `path_deny_list` as active only when a deny list is present, so selected staged writes are not classified as denied when no deny filter was requested. Verified against the three reported Kale migration reports: selected=4/actual=4 now simulates `extra=0`, `denied=0`, and `write_set_exact=true`; UE BuildPlugin succeeded and deployed DLL hashes match the packaged plugin.
- Kale Gap 2026-05-11 async job tools not exposed to Codex MCP surface: CLOSED/VERIFIED; remote tool async metadata is now normalized at ToolRegistry registration time, so schema exporters see `async` and `job_timeout_seconds` directly instead of depending only on MCP `tools/list` injection. Added unit coverage for registry-level async schema exposure, rebuilt debug and release `sage-server.exe`, verified stdio `tools/list` exposes `jobs.list/get/wait/logs/cancel` plus async metadata on long operations, and verified an async remote call returns `job_id` with `jobs.wait` state readback.
- Kale Gap 2026-05-11 async job status for long-running editor tools: FIXED in source; remote editor tools now advertise `async:true` and `job_timeout_seconds`, server strips async metadata before plugin dispatch, runs the editor tool in a detached job with correlated `job_id`, and stores final result/error plus structured logs. Added local `jobs.list`, `jobs.get`, `jobs.wait`, `jobs.logs`, and `jobs.cancel` tools with states `queued`, `dispatched`, `running`, `waiting_editor`, `completed`, `failed`, `cancel_requested`, and `editor_crashed`; editor disconnects during pending dispatch are classified as `editor_crashed`, normal completion after MCP detach remains retrievable by `job_id`, and local `index_slot` also supports `async:true`.
- Kale Gap 2026-05-11 asset.migrate_from_project animation skeleton repair regression: FIXED in source; migration skeleton verification now scans the destination package root for `UAnimationAsset` and `USkeleton` assets in addition to response written/relocation rows, uses `UAnimationAsset::SetSkeleton` with `PreEditChange`/`PostEditChangeProperty` and save/readback, reports root scan counts, and fails if any animation asset remains unresolved. Added `animation.set_animation_asset_skeleton` for explicit asset/list/directory repair with `repaired_assets[]`, `already_valid_assets[]`, and `failed_assets[]`; `editor.set_property` and `modify_asset_property` now route `UAnimationAsset.Skeleton` writes through `SetSkeleton` with readback diagnostics instead of silently accepting raw reflected writes that do not persist.
- Kale Gap 2026-05-11 asset.migrate_from_project ControlRig relocation conflict: FIXED in source; post-migration relocation now clears stale/invalid destination duplicates during overwrite with editor delete plus force-delete fallback diagnostics, moves the valid staged package into the requested destination root, fixes staged redirectors through AssetTools, reports `relocation_overwrite_delete_reports[]` and redirector fixup counts, `move_asset`/`rename_asset` gained explicit `overwrite:true` + `confirmed:true` replacement behavior with diagnostics, `asset.delete_batch` now returns per-path delete blocker diagnostics, and `index_slot` sanitizes duplicate/empty Asset/Class primary-key rows instead of failing the full Kuzu ingest.
- Kale Gap 2026-05-11 asset.migrate_from_project restart-stable animation skeleton references: FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; post-relocation migration now loads migrated `UAnimationAsset` packages, verifies Skeleton references point under the requested destination root, repairs missing/stale references with `UAnimationAsset::SetSkeleton` when a unique migrated `USkeleton` candidate exists, saves the repaired assets, reports `animation_skeleton_verification[]`/`animation_skeleton_repaired_assets[]`/`animation_skeleton_failures[]`, and fails migration if any animation skeleton remains unresolved. Also fixed live Kuzu `asset_renamed` deltas to delete+merge instead of mutating the `Asset.path` primary key.
- Kale Gap 2026-05-11 asset.migrate_from_project unresolved alias staged failure: FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; commandlet verification now classifies selected non-seed packages missing from the source AssetRegistry as unresolved alias/redirector candidates when AssetTools writes matching real packages with the same leaf name, reports them as `missing_selected_alias_assets[]`/`staged_missing_written_assets_all[]`, excludes them from blocking `missing_written_assets[]`, and allows actual written package relocation to continue.
- Kale Gap 2026-05-11 asset.migrate_from_project dependency subpath relocation: FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; post-migration relocation now uses the actual staged packages from `actual_written_assets[]` instead of requiring every selected alias package to exist, maps staged `/FlightCore/...` packages back to source-equivalent `/Game/...` paths, moves all real written packages into the requested destination root such as `/FlightCore/Animation/HeroFlight/SourceUE5`, and reports skipped non-written selected alias/redirector packages as `relocation_ignored_missing_selected_assets[]` without blocking relocation.
- Kale Gap 2026-05-11 asset.migrate_from_project actual AssetTools copy constraint: FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; source commandlets now pass `MigrationOptions.bIgnoreDependencies` by default so Unreal does not add dependencies beyond Sage's filtered selected set, snapshot/diff the destination Content root before and after migration, report `actual_written_assets[]`, `extra_written_assets[]`, and `denied_written_assets[]`, enforce path deny rules against actual disk writes, and reject plus rollback staged selected/unexpected packages if AssetTools still writes outside the selected set.
- Kale Gap 2026-05-11 asset.migrate_from_project UE5-only write verification: FIXED in source, packaged, and deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`; migration destinations now resolve into a true Content root plus requested package subpath, source commandlets write to the root expected by `AssetTools.migrate_packages`, subfolder/plugin destinations are relocated in the target editor with Unreal asset rename APIs, final `written_assets[]`/`missing_written_assets[]` are verified at the requested destination, and responses include `commandlet_log_path`/`commandlet_log_tail` plus staged relocation diagnostics.
- Kale Gap 2026-05-11 asset.migrate_from_project false-positive writes: FIXED in source, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, and verified by server build/tests, UE BuildPlugin, and matching deployed DLL hashes; `asset.migrate_from_project` now accepts explicit asset lists or source folders, expands folders through the source AssetRegistry, resolves plugin mount destinations as well as `/Game` and physical Content paths, exposes the dependency/class/path filters in the live schema, verifies destination `.uasset`/`.umap` files after migration, returns `written_assets[]`/`missing_written_assets[]` counts plus expanded/skipped readback, and marks non-zero commandlet exits or zero-write migrations as `success:false` unless writes were independently verified.
- Kale Gap 2026-05-10 run_pie local-player standalone session: FIXED in source, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, and verified by server build/tests, UE BuildPlugin, and matching deployed DLL hashes; `run_pie` now accepts transient PIE play-setting overrides for standalone/listen/client net mode, client count, selected viewport/new editor window, primary/local player index, and `force_local_player`, `editor.get_play_settings`/`get_pie_state` expose typed play-setting readback, and PIE input tools now fail with actionable no-`ULocalPlayer` diagnostics including current net mode/client count plus the suggested `run_pie` args.
- Kale Gap 2026-05-10 PIE input stale-world targeting: FIXED in source, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, and verified by server build/tests, live `tools/list`, UE BuildPlugin, and matching deployed DLL hashes; PIE input tools now resolve the same live PIE world context used by gameplay readback instead of stale `GEditor->PlayWorld`, accept explicit world/controller/pawn targets, return target/route diagnostics, schedule key releases against the same resolved target, and `gameplay.get_local_player` returns `world_path` plus PIE world candidates.
- Kale Gap 2026-05-10 selective external animation migration: FIXED in source, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, and verified by server build/tests, live `tools/list`, UE BuildPlugin, Python script syntax check, and matching deployed DLL hashes; `asset.migrate_from_project` now supports `include_dependencies:false`, hard/soft dependency toggles, class/path allow/deny filters, dry-run selected/skipped dependency reporting, and conflict-safe non-dry writes.
- Kale Gap 2026-05-10 diagnostic sweep: FIXED in source, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, and verified by server tests, live `tools/list`, UE BuildPlugin, and matching deployed DLL hashes; `editor.read_log`/`editor.search_log` now use Windows shared-read fallback with read diagnostics, and `gameplay.trace_input_action`/`gas.trace_ability_activation` capture Enhanced Input readback, pawn movement deltas, ASC owned tags/spec handles/spec summaries, reflected `ProcessAbilityInput`, and optional reflected `TryActivateAbility`.
- Kale Gap 2026-05-10 latest sweep: CLOSED/VERIFIED; all open gap-inbox items from this batch were implemented, packaged, deployed to `D:\Steamworks\Kale` and `D:\Steamworks\HeroFlight`, verified by Sage server/plugin builds, live `tools/list` metadata, matching deployed DLL hashes, and Kale dirty-asset readback.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; live schema now exposes `asset.migrate_from_project`, and the server registry build succeeded with the Phase 4 schema set.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; AnimSequence bone-track readback is frame-complete/range-aware, root-motion summaries are returned, and `animation.set_bone_keyframes` plus `animation.bake_root_motion_from_bone` are implemented through `IAnimationDataController`.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; added structured `animation.read_animation_curves`, implemented `animation.copy_animation_curves`, and implemented `animation.add_animation_modifier` with dry-run/apply/readback deltas.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; skeleton inspection now returns complete hierarchy, ref pose transforms, retarget modes, virtual bones, slot groups, and socket transforms; socket listing accepts skeleton and mesh assets consistently.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; IK Retargeter workflow is exposed through read, rig assignment, chain mapping, retarget-pose editing, batch retarget dry-run/conflict reporting, and implemented per-bone translation retargeting.
- Kale Gap 2026-05-10: CLOSED/VERIFIED; `lyra.set_default_gameplay_experience`, `level.create`/`level.load`, `level.get_actor_details`, `networking.get_info`, and `gameplay.project_to_nav` gaps were fixed with schema/handler alignment and structured readback/failure results.
- Kale Gap 2026-05-10: FIXED in source, deployed to Kale/HeroFlight, and verified by `LyraEditor` + `HeroFlightEditor` builds; `stop_pie`, `level.load`, and `editor.cleanup_python_refs` now purge Python-held PIE/editor UObject wrappers via `PrepareToCleanseEditorObject`, clear optional Python globals, and run Python/UE GC before teardown/load.
- Kale Gap 2026-05-10: FIXED in source, deployed to Kale/HeroFlight, and verified by project editor builds; added PIE input simulation tools `input.press_key`, `input.hold_key`, `input.release_key`, and `input.trigger_action` for viewport/PlayerController key events and Enhanced Input action injection with deterministic holds.
- Kale Gap 2026-05-10: FIXED in source and verified by builds; `gas.create_ability` now honors native/Blueprint GameplayAbility parent classes and errors on invalid/non-ability parents instead of silently falling back to `UGameplayAbility`.
- Kale Gap 2026-05-10: FIXED in source and verified by builds; added typed `asset.clear_array_property` and `asset.replace_array_property` with dry-run/validate-only, removed/added reporting, recursive FProperty coercion, and dirty-on-real-mutation behavior.
- Kale Gap 2026-05-10: FIXED in source and verified by builds; added `asset.migrate_from_project`, which launches the source UE project commandlet, computes AssetRegistry dependency closure, supports dry-run/conflict reporting/overwrite gating, and invokes Unreal `AssetTools.migrate_packages` rather than manual file copying.
- Kale Gap 2026-05-10: FIXED in source and deployed to Kale/HeroFlight; `level.load` now normalizes map paths, uses the editor map-loading API, polls `GEditor` world readback, and falls back to opening the UWorld asset for mounted GameFeature/plugin maps.
- Kale Gap 2026-05-10: FIXED in source and deployed to Kale/HeroFlight; `animation.add_linked_anim_layer_node` now exposes scalar ALI call-site custom pins (AimYaw/AimPitch style) and `animation.bind_anim_node_property` binds them with UE's generated custom-property binding keys.
- Kale Gap 2026-05-10: FIXED in source and deployed to Kale; added `animation.set_layer_function_input_pins` plus `animation.add_layer_function` `pins` support for ALI `UAnimGraphNode_LinkedInputPose` pose/scalar parameters.
- Gap #30: FIXED in source; `asset.add_array_element` hardened with dry-run/validation and dedicated GameFeature component-entry tools added.
- Gap #29: FIXED in source; added `animation.set_transition_automatic_rule` for `UAnimStateTransitionNode` automatic sequence-player rules.
- Gap #28: FIXED; `animation.add_linked_anim_layer_node` layer binding/title drift was repaired and made explicit.
- Gap #27: FIXED; `bp.add_interface` no longer silently corrupts existing AnimLayerInterface linked-layer nodes.
- Gap #26: FIXED; `bp.rename_function` no longer breaks LinkedAnimLayer node Layer properties project-wide.
- Gap #25: FIXED; anim node spawn tools can target interface override graphs through graph-name lookup.
- Gap #24: FIXED; AnimLayerInterface child override graph spawn and master AnimGraph linked-layer call support added.
- Gap #23: CLOSED/VERIFIED on Windows; `restart_editor` resolves real editor targets from `Source/*.Target.cs` / `.uproject`, invokes quoted `Build.bat` through `call`, deploys full packaged plugin contents, and reports `editor_terminated` in rebuild-failure error data.
- Gap #22: CLOSED/VERIFIED in Lyra; UE 5.7 anim node property binding and readback support works for FlightCore blendspace bindings.
- Gap #21: FIXED; AnimBlueprint state-machine initial state, transition rules, expression rules, and transition readback implemented and dogfooded.
- Gap #20: FIXED; Phase 4-r6 anim suite shipped broad animation tool coverage across primitive, convenience, runtime, and validation clusters.
- Gap #19: FIXED; root motion source runtime tools implemented, including add/remove support.
- Gap #18: FIXED; `animation.create_anim_notify` and notify-state creation implemented.
- Gap #17: FIXED; blendspace sample add/set/delete support implemented with structured skipped entries.
- Gap #16: FIXED; state-machine handler suite implemented with canonical graph/subgraph registration.
- Gap #15: FIXED; `gameplay.set_world_game_mode` schema/runtime mismatch corrected and destructive confirmation added.
- Gap #14: FIXED; `level.create` Niagara/editor-world crash addressed through canonical `UWorld::CreateWorld` setup.
- Gap #13: FIXED; native inherited component class override support added for components such as `CharMoveComp`.
- Gap #9 regression: FIXED; `project.add_module_dependency` now matches exact quoted module entries instead of substrings.
- Gap #pre-13: CLOSED; tools/list empty registry issue fixed by replacing invalid JSON-null schema property values.
- Gap #P8-KaleGame-2026-05-07: FIXED in source; BP duplicate aliases, CDO hard object references, and plugin-path asset validation were corrected.
- Gap - BP SCS component authoring + GameFeature ComponentList: FIXED in source; added `bp.add_component`, `bp.copy_component_defaults`, and GameFeature component-entry tools.
- Gap - IMC authoring safety + editor Python crash surfaces: FIXED in source; typed IMC edit tools and unsafe Python mutation rejection added.
- Gap - Level loading and Lyra WorldSettings experience setter: FIXED in source; `level.load`, `level.set_world_settings`, and `lyra.set_default_gameplay_experience` hardened with readback/verification.
- Gap - gameplay.set_imc_mapping_key crash: FIXED in source; rebind now uses typed remap path with dry-run/readback and preserves triggers/modifiers.
- Closed sweep 2026-05-04: CLOSED; 7 inbox gaps plus one silent regression closed, 72 net tools added, destructive confirmations and stub honesty rules hardened.

## Open Detailed Gaps

### Sage Gap 2026-06-04 Niagara post-reference benchmark parity deltas

Status: OPEN - REFERENCE AUDIT CONFIRMED; IMPLEMENTATION PENDING

Blocked task:
- The 2026-06-04 external benchmark audit confirmed that Sage's new 68-tool Niagara surface is no longer stubbed, but Monolith, UE-MCP, and ChiR24 still expose several high-value authoring and discovery behaviors Sage does not yet implement. These are the remaining Niagara parity deltas after the source sweep, not regressions in the implemented 68-tool pass.

Verified references:
- `tumourlove/monolith` Tool Reference is the strongest Niagara-specific benchmark found. It documents `niagara_query` with 129 actions, including direct existing `CustomHlsl` read/write, HLSL module/function creation, dynamic-input lifecycle, event-handler and simulation-stage authoring, stack usage selectors, temporal controls, stateless emitter creation, search/discovery actions, and preview/GIF capture.
- `db-lyon/ue-mcp` exposes a 26-action Niagara category with system/emitter CRUD, renderer CRUD, data-interface inspection, compiled HLSL, module input/static-switch access, HLSL module creation, scratch module creation, system-from-spec, and batch.
- `ChiR24/Unreal_mcp` documents Phase 12 Niagara preset authoring for spawn modules, initialize/force/velocity/size/color/collision/kill/camera-offset modules, renderer presets, data-interface adders, event generator/receiver, GPU simulation enable, simulation stage, and validation.
- Flopperam, runreal, remiphilippe, UECortex, and Codeturion unreal-api-mcp were checked as secondary product/API references; none displaced Monolith as the Niagara feature benchmark.

Observed remaining Sage deltas:
- `niagara.create_module_from_hlsl`, `niagara.create_scratch_module`, `niagara.set_dynamic_input`, `niagara.set_custom_expression`, `niagara.add_event_handler`, `niagara.add_simulation_stage`, and `niagara.capture_preview` currently return hard unsupported errors. This is honest behavior, but it is still a parity gap.
- Sage has no Niagara-specific search/discovery pack equivalent to Monolith's parameter/data-interface/material/system-query/similarity/reference search actions.
- Sage has basic graph and module read/write, but not Monolith-style ordered stack usage selectors for `particle_event` and `particle_simulation_stage`, dynamic-input lifecycle, HLSL `CustomHlsl` node read/write, or stack-context response shaping.
- Sage can inspect cached data interfaces and audit dependencies, but lacks first-class data-interface add/configure helpers and DI function listing.
- Sage renderer CRUD exists, but still needs renderer binding/material convenience helpers to make common sprite/mesh/material tuning agent-friendly.
- Sage has preview spawn and SimCache-style numeric validation paths, but no deterministic asset preview image/GIF capture pipeline.

Expected behavior:
- Implement source-original, UE-API-backed support for existing `CustomHlsl` node text read/write and HLSL module/function/scratch authoring where exported APIs allow it; otherwise split read-only discovery from explicit unsupported write boundaries.
- Add Niagara search/discovery tools: search by parameter, data interface, material, structural query, similar systems, cross-asset Niagara references, and system-level data-interface listing.
- Extend stack selectors so module/input operations can target system/emitter/particle/event/simulation-stage usages and usage ids without ambiguity.
- Add dynamic-input lifecycle support: list/search/add/set/remove dynamic inputs with readback of linked function calls and override pins.
- Implement event-handler and simulation-stage creation with full construction contracts, validation, compile/save/readback, and rollback on failure.
- Add first-class DI add/configure helpers for common cases such as skeletal/static mesh, spline, audio spectrum, and collision query data interfaces.
- Add renderer binding/material helpers for common sprite/mesh renderer fields and material parameter binding, preserving the generic reflected property writer.
- Add preview image/GIF capture only when Sage owns a deterministic render-target/viewport capture path; keep `preview_spawn` + SimCache as the numeric fallback.

Acceptance criteria:
- The living matrix at `.claude/notes/niagara-mcp-reference-matrix.md` remains the source-backed benchmark map and links every new Niagara implementation slice to Monolith, UE-MCP, ChiR24, or a UE-header-only design note.
- No public permissive or third-party licensed code is copied into Sage; implementations stay source-original and Unreal API backed.
- New writer tools support `dry_run`, `validate_only`, `save`, `compile`, before/after/readback, GameThread execution, transaction scope, and clear mutation ownership.
- `scripts/audit-tools.ps1` and registry tests continue to show schema/handler alignment and zero successful placeholder stubs.
- A live Kale duplicate Niagara asset can prove at least one HLSL/custom-expression readback, one dynamic-input operation, one search/discovery query, one event/sim-stage operation, and one preview evidence path after build/package/deploy is allowed.

Implementation notes:
- Do not reopen the already closed 2026-06-03 Niagara source sweep. Treat this as the next parity tranche found by the external reference audit.
- Prefer small, verifiable slices ordered by value: search/discovery first, stack selectors and dynamic inputs second, HLSL/custom nodes third, event/sim-stage creation fourth, visual capture last unless a live dogfood task specifically needs it.

### Sage Gap 2026-06-03 Niagara editor API adapter, transaction, compile, and schema honesty

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Niagara authoring work is now large enough that one-off reflected UObject writes are the wrong layer. Kale FlightCore thruster work needs Sage to edit systems, emitters, module stacks, user parameters, renderers, bounds, and preview components through the same editor/runtime contracts the Niagara editor uses, then compile/save/read back honestly.

Observed gaps:
- `server/src/tools/phase4_schemas.cpp` advertises 26 `niagara.*` tools, but many handlers in `plugin/Source/SageBridge/Private/Tools/SageNiagaraTools.cpp` still return successful note-only responses such as "requires NiagaraEditorModule" for add emitter, emitter property mutation, renderer CRUD, module input mutation, static switches, compiled HLSL, scratch modules, and HLSL module creation.
- UE 5.7 exposes the needed editor surface through `NiagaraEditorModule.h` (`RequestCompileSystem`, `PollSystemCompile`, existing system view model lookup, renderer creation registry, data-interface feedback, cached parameter collections), `NiagaraEditorUtilities.h` (`AddEmitterToSystem`, `RemoveEmittersFromSystemByEmitterHandleId`, parameter add helpers, available parameter collections), and `NiagaraSystemViewModel.h` data-processing options. Sage has no common adapter around those APIs yet.
- There is no shared Niagara transaction/compile/save/readback pipeline. Each future writer would otherwise have to rediscover GameThread enforcement, `FScopedTransaction`, dirty-package handling, compile request/polling, `validate_only`, `dry_run`, schema readback, and undo safety.

Expected behavior:
- Add a dedicated editor-only Niagara backend layer, e.g. `SageNiagaraEditorAdapter`, loaded only when Niagara/NiagaraEditor modules are available. It should resolve system/emitter/component targets, create or reuse a data-processing `FNiagaraSystemViewModel`, run writes on the GameThread, wrap real mutations in `FScopedTransaction`, and centralize dirty/save/compile/readback behavior.
- Add a strict support matrix for every advertised `niagara.*` schema entry: implemented, read-only, unsupported with hard error, or hidden from `tools/list`. A placeholder note must not count as success.
- Add compile helpers that call the Niagara editor compile path, poll or return async handles intentionally, and surface compile status, warnings/errors, affected scripts, and whether readback came from post-compile state.
- Add module dependency checks in `SageBridge.Build.cs`/`.uplugin` for Niagara runtime/editor modules and deterministic failure when the editor module is missing.

Acceptance criteria:
- `niagara.add_emitter`, `niagara.set_emitter_property`, `niagara.add_renderer`, `niagara.set_renderer_property`, `niagara.set_module_input`, `niagara.set_static_switch`, `niagara.create_module_from_hlsl`, and `niagara.create_scratch_module` no longer return successful placeholder notes.
- Every Niagara writer supports `dry_run`, `validate_only`, `save`, `compile`, before/after/readback, and clear mutation scope (`system-local`, `shared-emitter-asset`, `component-runtime`, or `collection-global`).
- `scripts/audit-tools.ps1` and unit registry tests prove server schema, plugin handlers, and advertised behavior are aligned.
- A duplicate Kale Niagara asset can be modified, compiled, saved, reloaded, and read back without opening the Niagara UI by hand.

Implementation notes:
- Start by wrapping UE source points already verified locally: `INiagaraEditorModule::RequestCompileSystem`, `FNiagaraEditorUtilities::AddEmitterToSystem`, `FNiagaraSystemViewModelOptions::bIsForDataProcessingOnly`, and `FNiagaraStackGraphUtilities` stack helpers.

### Sage Gap 2026-06-03 Niagara graph, stack context, dynamic input, and script-node authoring parity

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Thruster shaping and future VFX authoring will require real graph/stack edits, not only asset duplication or reflected top-level property writes. Examples: replace a cylindrical location module with a box-style module, bind a module input to a user parameter, add a dynamic input, set a custom expression, or target a simulation-stage/event script instead of the normal particle update stack.

Observed gaps:
- Existing `niagara.list_modules`, `niagara.list_module_inputs`, and `niagara.set_module_input` do not traverse `UNiagaraGraph` / `UNiagaraNodeFunctionCall` stack data. They cannot identify stack usage, usage id, output node, module order, override pins, hidden/static inputs, linked parameters, dynamic inputs, or inherited/local ownership.
- UE 5.7 has the relevant public stack utility surface in `NiagaraStackGraphUtilities.h`: ordered module nodes, all sim-stage/event-handler module nodes, stack context resolution, parameter-map history, stack function inputs/static-switch pins, override pin creation, linked parameter writes, data-interface/object writes, dynamic input assignment, custom expression assignment, and module removal.
- There are no public Sage tools for `niagara.add_module`, `niagara.remove_module`, `niagara.move_module`, `niagara.duplicate_module`, `niagara.list_dynamic_inputs`, `niagara.set_dynamic_input`, `niagara.set_custom_expression`, or graph export/import.

Expected behavior:
- Implement stack-aware readers that return modules by emitter, usage (`system_spawn`, `system_update`, `emitter_spawn`, `emitter_update`, `particle_spawn`, `particle_update`, `particle_event`, `particle_simulation_stage`), usage id, order, script asset, node GUID, inherited/local state, enable state, and warnings.
- Implement module input readers/writers through Niagara stack graph utilities, including literal values, linked `User.*` parameters, object/material/class refs, data interfaces, dynamic inputs, custom expressions, and static switches.
- Add module stack mutation tools with dry-run/readback: add, remove, move, duplicate, replace, and reset override.
- Add graph read/export tools for `UNiagaraScriptSource` / `UNiagaraGraph` that can dump nodes, pins, links, parameter-map flow, and output usage without requiring a screenshot of the Niagara editor.

Acceptance criteria:
- Against a Kale RocketThruster duplicate, Sage can list the actual ordered modules for every emitter and usage, including node GUIDs and input summaries.
- Sage can bind a numeric module input to `User.ThrusterIntensity`, then read the override pin and compile result back.
- Sage can add or replace a shape/location module on a duplicate asset and report whether the new module targets particle spawn/update or another usage.
- Event-handler and simulation-stage stacks are addressable by stable selectors, not confused with normal particle update modules.

Implementation notes:
- The tool surface should mirror Monolith's strongest Niagara concepts where practical: module stack actions, dynamic input lifecycle, simulation-stage/event selectors, HLSL/custom expression read/write, and structured graph export.

### Sage Gap 2026-06-03 Niagara Parameter Collection and global VFX profile tooling

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Some future FlightCore and gameplay VFX tuning may be better as global or profile-scoped Niagara Parameter Collections rather than per-component user parameters. Examples: shared heat haze strength, global flight VFX quality scale, environment wind/turbulence, or a developer tuning collection that affects multiple thruster systems at once.

Observed gaps:
- Sage has no Niagara Parameter Collection tools. It cannot list/create/update `UNiagaraParameterCollection`, read or mutate default collection instances, bind systems/modules to collection parameters, or set runtime collection instance values.
- UE 5.7 exposes collection discovery and usage points: `FNiagaraEditorUtilities::GetAvailableParameterCollections`, `INiagaraEditorModule::FindCollectionForVariable`, `UNiagaraSystem::UsesCollection`, `UNiagaraFunctionLibrary::GetNiagaraParameterCollection`, and world/system instance collection access.
- Current user parameter work only covers `User.*`; collection variables need separate ownership, namespace, default-instance, runtime-instance, and asset dependency handling.

Expected behavior:
- Add `niagara.collection.list`, `niagara.collection.read`, `niagara.collection.create`, `niagara.collection.upsert_parameter`, `niagara.collection.set_default`, and `niagara.collection.set_runtime_value`.
- Add readback for systems that use a collection: collection asset path, variable names/types/defaults, runtime override values where a world exists, and modules/renderers that reference collection variables.
- Support dry-run/save/compile/readback and reject global collection mutation without explicit confirmation when the collection is shared across many assets.

Acceptance criteria:
- Sage can create a test collection with scalar/vector/color values, bind a Niagara module input to one collection variable, save/compile, and report `UNiagaraSystem::UsesCollection == true`.
- Sage can set a runtime collection value in PIE/editor preview and prove the active Niagara component sees the updated value or report a precise reason it cannot.
- Shared collection writes return referencer diagnostics before mutation.

### Sage Gap 2026-06-03 Niagara effect type, scalability, fixed bounds, warmup, and culling diagnostics

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Flight thruster effects must not disappear, cull incorrectly, or become expensive when multiplied across characters. GPU emitters, long plumes, and fast-moving actors need fixed bounds, warmup, effect type/scalability awareness, and culling diagnostics as first-class authoring tools.

Observed gaps:
- Existing Sage Niagara tools do not read or write `UNiagaraSystem` fixed bounds, component runtime fixed bounds, warmup settings, effect type, scalability settings, max instance limits, significance/culling rules, or GPU-sim missing-bounds warnings.
- UE 5.7 runtime/editor source exposes these concerns separately: `UNiagaraComponent::SetSystemFixedBounds`, `SetEmitterFixedBounds`, warmup override fields, `UNiagaraSystem::GetEffectType`/`SetEffectType`, system scalability settings, and `FNiagaraWorldManager` culling paths that depend on fixed bounds, effect type, distance, visibility, instance count, and budget.
- External references show this is not optional: mature Niagara MCP surfaces include `set_fixed_bounds`, effect type/scalability writers, and validation warnings for GPU+bounds/culling issues.

Expected behavior:
- Add `niagara.read_scalability`, `niagara.set_scalability`, `niagara.set_fixed_bounds`, `niagara.clear_fixed_bounds`, `niagara.set_warmup`, `niagara.set_effect_type`, and `niagara.validate_system`.
- Validation should detect missing fixed bounds for GPU emitters, suspicious dynamic bounds on fast-moving effects, effect type culling conflicts, missing materials, disabled emitters, zero spawn rate, missing user parameters, and compile errors.
- Preview/component readers should include bounds, warmup, scalability/effect type, active/cull state, and warnings when an effect is invisible because of bounds or culling rather than because it failed to spawn.

Acceptance criteria:
- Against a RocketThruster duplicate, Sage can set system fixed bounds, warmup ticks/delta or warmup time, and read them back after save/reload.
- A validation run reports clear warnings for GPU emitters without fixed bounds and for systems whose effect type/scalability would cull them in the current preview.
- The preview tool can distinguish "component spawned but culled/out of bounds" from "component never activated" and "system asset missing dependency."

### Sage Gap 2026-06-03 Niagara SimCache, temporal capture, event-handler, simulation-stage, and data-channel diagnostics

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Visual screenshots are not enough for future Niagara work. We will need temporal and structural evidence for hover vs fast-flight profiles, bursts, event-driven smoke, GPU simulation stages, and data-interface/data-channel effects.

Observed gaps:
- There is no Sage tool to capture/read a Niagara SimCache, sample frames over time, inspect active emitters/particle counts across frames, or compare profile A vs profile B numerically.
- There are no event-handler or simulation-stage tools, even though advanced Niagara MCP references expose event handler CRUD, simulation stage CRUD, and selectors for `particle_event` / `particle_simulation_stage` stacks.
- Data-channel and data-interface diagnostics are currently limited to placeholder notes. Sage cannot tell whether an event, data channel, or DI path is producing inputs for the system.

Expected behavior:
- Add `niagara.capture_sim_cache`, `niagara.read_sim_cache`, and `niagara.compare_sim_cache` for preview-spawned systems, with frame/time sampling, emitter names, particle counts, bounds, and selected parameter values.
- Add `niagara.list_event_handlers`, `niagara.add_event_handler`, `niagara.remove_event_handler`, `niagara.list_simulation_stages`, `niagara.add_simulation_stage`, and `niagara.remove_simulation_stage`.
- Add structured diagnostics for data-channel/data-interface participation: DI class, owner, bound variables, compile usage, runtime availability, and errors/warnings from Niagara editor feedback APIs.

Acceptance criteria:
- Sage can run two short previews of a RocketThruster duplicate (`HoverSoft` and `FastBoost`), capture temporal readback, and show that the fast profile has different bounds/intensity/particle metrics or report why those metrics are unavailable.
- Sage can list event handlers and simulation stages on a system/emitter without conflating them with normal particle spawn/update modules.
- Data-interface validation returns real ownership and feedback data, not a placeholder note.

### Sage Gap 2026-06-03 Niagara asset dependency, redirector, cook, and GameFeature mount validation

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- RocketThrusterExhaustFX was moved/reimported under FlightCore and texture/material dependencies broke during the workflow. Future Niagara edits need asset-level validation before and after moves, duplicates, reimports, and GameFeature packaging.

Observed gaps:
- Sage cannot currently audit a Niagara system's full dependency graph: emitter assets, renderer materials, material textures, data interfaces, parameter collections, external plugin roots, redirectors, missing packages, stale asset registry entries, or C++-only references.
- There is no Niagara-specific "safe duplicate/move/make local" flow that fixes redirectors, re-saves dependent packages, validates mounted plugin paths such as `/FlightCore`, and verifies content is cookable.
- Current tools cannot answer whether a broken VFX came from Niagara stack logic, missing texture/material dependency, shared emitter mutation, redirector damage, GameFeature mount issues, or package load failure.

Expected behavior:
- Add `niagara.get_dependencies`, `niagara.find_referencers`, `niagara.validate_dependencies`, `niagara.make_emitters_local`, and `niagara.audit_cross_asset_refs`.
- Dependency validation should include renderer material paths, texture sources, data interfaces, parameter collections, emitter handles, shared/local ownership, missing/redirector packages, plugin mount root, dirty state, and cook/data-validation results where available.
- Add a duplicate/move workflow that can preserve source package references, optionally make shared emitters local, fix redirectors, save all affected assets, and return before/after dependency readback.

Acceptance criteria:
- Against `/FlightCore/RocketThrusterExhaustFX`, Sage can report every Niagara system's emitter/material/texture dependencies and flag missing or redirector-backed packages.
- A duplicate variant operation returns whether it shares or owns each emitter and renderer dependency.
- A validation run after moving or reimporting the package identifies texture/material breakage before runtime testing.

### Sage Gap 2026-06-03 Unreal MCP Niagara reference parity matrix and adoption audit

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- The web/GitHub scan found multiple Unreal MCP projects with Niagara/VFX claims. Sage needs a durable reference matrix so Niagara gaps are implemented against the best current public patterns instead of reinventing or under-scoping the tool surface.

Observed external references from web/GitHub scan on 2026-06-03:
- `tumourlove/monolith`: best Niagara-specific reference found. Public docs list a native C++ Unreal 5.7 MCP with a `niagara_query` namespace, 129 Niagara actions, module stacks, dynamic inputs, event handlers, simulation stages, renderers, scalability, HLSL module creation/editing, preview capture/GIF tooling, search/discovery, validation, and cross-asset reference audits. License is MIT; source/design should be audited first.
- `db-lyon/ue-mcp`: broad and actively maintained reference. Docs list 21 category tools / 513+ actions and a Niagara category covering list/get_info/spawn/set_parameter/create emitter/add emitter/list renderers/module inputs/static switches/HLSL/batch. Useful for ergonomics, setup, handler conventions, feedback loops, and runtime parameter/component flows. License is BUSL/commercial for some usage, so treat as reference unless licensing is cleared.
- `ChiR24/Unreal_mcp`: higher-star broad MCP reference. README claims native C++ automation bridge, Niagara particles/GPU simulations/procedural effects/debug shapes, and Blueprint/Niagara/Material/BehaviorTree graph manipulation. Useful for transport/plugin dependency gating and broad schema ideas.
- `flopperam/unreal-engine-mcp`: hosted/full product claims VFX tools (`niagara_inspect`, `niagara_edit`, `niagara_script_edit`) plus many domains; the open-source local repo is described as simpler/foundational. Useful for hosted/onboarding/product shape, less useful as direct open Niagara implementation unless the full tool source is available.
- `db-lyon/ue-mcp` website/docs: useful as product documentation for VFX category and end-to-end setup, including `npx ue-mcp init`, bridge deployment, and plugin enablement.
- `runreal/unreal-mcp`: Python Remote Execution MCP. Useful as a minimal fallback/reference for arbitrary Python execution and asset export, but not enough for protected Niagara editor internals.
- `remiphilippe/mcp-unreal`: Go/Remote Control based Unreal 5.7 MCP with 49 tools and API-doc lookup. Useful for build/test/editor control and docs indexing patterns; no strong Niagara authoring evidence found in the scan.
- `UECortex` marketplace listing: claims a native UE5 embedded MCP server with 139 pure C++ tools across 13 categories including Niagara. GitHub/source location was not verified in this scan; keep as a candidate to locate and evaluate.
- `Codeturion/unreal-api-mcp`: API lookup MCP for UE 5.5/5.6/5.7 including built-in plugins such as Niagara. Useful as an API documentation supplement, not an editor authoring implementation.

Expected behavior:
- Create a living `docs/research/niagara-mcp-reference-matrix.md` or equivalent that compares Sage vs Monolith vs UE-MCP vs ChiR24 vs fallback projects by feature: user parameters, runtime component parameters, system/emitter CRUD, module stack, module inputs, dynamic inputs, static switches, renderers, HLSL/custom nodes, data interfaces, event handlers, simulation stages, SimCache/preview, scalability/fixed bounds, dependency audits, batch/transactions, compile/save/readback, and licensing constraints.
- For Monolith specifically, audit the public source and wiki for implementation patterns that can legally and technically inform Sage: namespace dispatch, response shaping, stack selectors, ParameterMap bridge, HLSL custom node editing, renderer/material search, validation, and preview capture.
- For UE-MCP/ChiR24, audit schema/handler designs and decide which ergonomics should be adopted, not copied blindly.
- Track source links, license notes, last release/activity, stars only as weak popularity signals, and exact features verified from docs/source.

Acceptance criteria:
- A reference matrix exists and identifies Monolith as the current best Niagara-specific benchmark unless a stronger source-backed candidate is found.
- Each Sage Niagara gap maps to one or more external references or explicitly says "no external implementation found; design from UE headers."
- The matrix includes a licensing note for every candidate before any implementation is borrowed.
- The gap inbox links back to the matrix once created, and future Niagara implementation PRs can cite which reference feature they are closing.

### Sage Gap 2026-06-03 Niagara user-parameter authoring and runtime component parameter parity

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Kale FlightCore thruster profiles need the same Niagara system to react to gameplay state: hover should be dim/short/slow, fast flight should be longer and harder, boost/dodge should spike intensity. That requires authoring and wiring Niagara user parameters such as `User.ThrusterIntensity`, `User.ThrusterLengthScale`, `User.SpawnRateScale`, `User.VelocityScale`, `User.HeatHazeScale`, and `User.SmokeScale`, then setting those parameters on the spawned Niagara components from the GameplayCue at runtime.

Observed gaps:
- `niagara.set_parameter` schema says it sets a user-exposed parameter on a spawned Niagara component with `component/name/value`, but the plugin implementation currently expects `path/name`, loads an asset, and tries to set a reflected UObject property. It does not target a spawned `UNiagaraComponent`, does not call Niagara component `SetVariable*` APIs, and does not return component parameter readback.
- `niagara.list_system_parameters` is advertised as user-exposed parameter listing, but it currently only scans UObject properties whose names start with `Niagara` or contain `Parameter`; it does not read a `UNiagaraSystem` user parameter store, default values, parameter namespaces, or usages.
- There is no tool to create/update user parameters on a system/emitter, set default values, bind a module input to `User.*`, or prove that a parameter is actually consumed by an emitter/module/renderer.

Expected behavior:
- Add real system/emitter user parameter readback, either as `niagara.list_user_parameters` / `niagara.read_parameters` or by making `niagara.list_system_parameters` honest. Return name, namespace, type, default value, exposed flag, editor sort/group metadata where available, and usage summary.
- Add `niagara.upsert_user_parameter` with typed values for float, int, bool, enum, `FVector2f`, `FVector3f`, `FVector`, color, object/class/asset refs, and data interfaces. Support `dry_run`, `validate_only`, `save`, `compile`, and before/after readback.
- Add `niagara.bind_module_input_to_parameter` or make `niagara.set_module_input` support parameter-link value mode so a module input can be wired to `User.ThrusterIntensity` instead of a literal value.
- Fix `niagara.set_parameter` so it really targets a spawned `UNiagaraComponent` or Niagara actor/component path, calls the correct `SetVariable*` API, and returns readback/validation.

Acceptance criteria:
- Against Kale `NS_RocketExhaust_Blue`, Sage can create/read/update `User.ThrusterIntensity`, `User.ThrusterLengthScale`, `User.SpawnRateScale`, `User.VelocityScale`, `User.HeatHazeScale`, and `User.SmokeScale` on a duplicate asset.
- Sage can bind at least one emitter module input and one renderer/material-facing input to a `User.*` parameter and save/compile the asset.
- Sage can spawn the Niagara system in an editor/PIE preview, set those user parameters on the spawned component, and read back the values from the component.
- Schema and implementation must agree: `niagara.set_parameter` should no longer accept a schema shape that the plugin does not implement.

### Sage Gap 2026-06-03 Niagara preview, simulation, and visual validation tools

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- The rectangular thruster attempt failed partly because Sage could not objectively verify the rendered effect. Future Niagara work needs a way to spawn a preview, set user parameters, advance/simulate briefly, and return enough evidence to decide whether the effect is short/long, dim/bright, round/rectangular, active/inactive, or blank.

Observed gaps:
- `niagara.spawn` returns only an actor id and system path. It does not return the Niagara component id, active emitter names, bounds, parameter values, asset dependency diagnostics, or a way to clean up the preview actor deterministically.
- There is no Niagara-specific preview capture, thumbnail render, component bounds readback, active particle count/readback, or "is this component active and rendering" diagnostic.
- There is no batch preview flow that can compare two profiles such as `HoverSoft` and `FastBoost` with the same system and different user parameters.

Expected behavior:
- Extend `niagara.spawn` or add `niagara.preview_spawn` to return actor id, component id, world, system, transform, active state, bounds, and cleanup token. Respect `auto_destroy` honestly.
- Add `niagara.set_component_parameters` for batch runtime parameter writes to a preview component.
- Add `niagara.read_component` to return system, active emitters if available, bounds, component transform, parameter overrides, scalability/significance state, and warnings if the system is inactive or not rendering.
- Add `niagara.capture_preview` or equivalent to render a viewport/thumbnail image for the spawned effect after a configurable warmup duration and save it to a requested path.
- Add deterministic cleanup for preview actors/components created by these tools.

Acceptance criteria:
- Sage can preview `NS_RocketExhaust_Blue` with a `HoverSoft` parameter set and a `FastBoost` parameter set, capture two screenshots or thumbnails, and return component bounds/active-state readback for both.
- A preview component can be cleaned up without leaving dirty levels or orphan actors.
- The tool reports failure when an effect is blank, inactive, missing a system, missing required user parameters, or culled by scalability.

### Sage Gap 2026-06-03 Niagara declarative system/emitter creation from spec

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Future VFX tasks should not depend on manually building every Niagara system in the editor. For simple effects like thruster exhaust, impact bursts, UI pulses, contrails, smoke puffs, and debug probes, Sage should be able to create a working Niagara system/emitter from a declarative spec and then let artists tune it.

Observed gaps:
- `niagara.create_system_from_spec` is advertised as a declarative JSON emitter spec tool, but the plugin implementation currently delegates to `niagara.create` and does not consume the spec.
- `niagara.create` and `niagara.create_emitter` can create empty assets, but they do not create emitters, add modules, configure renderers, set materials, add user parameters, compile, or return useful readback.
- `niagara.batch` is currently a placeholder note response, so callers cannot use it as a reliable atomic multi-step authoring operation.

Expected behavior:
- Implement `niagara.create_system_from_spec` for at least sprite-based systems with emitters, spawn/update modules, renderer setup, materials, user parameters, simple curves/distributions, bounds, local-space flag, and scalability basics.
- Support safe dependency resolution for materials, textures, existing Niagara modules, and emitters.
- Support `dry_run`, `validate_only`, `overwrite`/conflict diagnostics, `save`, `compile`, and full readback.
- Make `niagara.batch` a real sequential transaction helper or remove it from schema until it exists.

Acceptance criteria:
- A JSON spec can create a minimal sprite exhaust system with one emitter, one sprite renderer, one material, a spawn-rate module, a velocity module, size/color over life, and user parameters for intensity/length.
- The created system compiles, saves, appears in AssetRegistry, and can be spawned by the preview tool.
- Failures are structured: missing material, invalid module name, unsupported input type, compile error, and asset conflict are each reported without dirtying unrelated assets.

### Sage Gap 2026-06-03 Niagara data-interface, scratch-module, static-switch, and compiled-HLSL parity

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- More advanced future effects will need data interfaces, scratch-pad modules, static switches, custom HLSL modules, and compiled-output diagnostics. This matters for procedural thruster shaping, GPU effects, mesh/sample-driven emissions, gameplay-driven curves, and debugging why a module binding does not affect the rendered result.

Observed gaps:
- `niagara.inspect_data_interfaces` returns only a note and does not inspect data interfaces attached to a system/emitter/module.
- `niagara.create_scratch_module` returns the same placeholder as HLSL module creation and does not create or attach a scratch-pad module.
- `niagara.create_module_from_hlsl` returns only a note and does not create a usable Niagara script module.
- `niagara.list_static_switches` and `niagara.set_static_switch` are note-only placeholders.
- `niagara.get_compiled_hlsl` is advertised but returns only a note; there is no compile status, VM/HLSL output, or diagnostics readback.

Expected behavior:
- `niagara.inspect_data_interfaces` must return data-interface class, outer/module ownership, editable properties, current values, default values, and whether the DI is system/emitter/module scoped.
- Add data-interface property mutation with dry-run/save/compile/readback for common DI types where UE editor APIs permit it.
- Implement scratch module creation/attachment with inputs, outputs, HLSL or graph operations where practical, and clear unsupported-feature errors where not.
- Implement static switch read/write with before/after and compile diagnostics.
- Implement compiled HLSL/VM diagnostic readback for target scripts or return a hard error if UE private API access is unavailable; do not return success with only a note.

Acceptance criteria:
- Sage can inspect and report data interfaces used by a Niagara emitter in a real project asset.
- Sage can create or attach a simple scratch module, set at least one input, compile, save, and read it back.
- Sage can read/write a static switch on a duplicate asset and prove the compiled output changes or reports a meaningful compile diagnostic.
- Stub success responses are eliminated for these advertised advanced Niagara tools.

### Sage Gap 2026-06-03 Niagara stack/module authoring for emitter spawn shapes

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Kale dogfood needed to turn `/FlightCore/RocketThrusterExhaustFX/FX/NS_RocketExhaust_Blue_Rect.NS_RocketExhaust_Blue_Rect` from a rounded/cylindrical exhaust into a true rectangular thruster plume without hand-editing the Niagara editor. The current Sage Niagara surface could duplicate the system asset, but could not inspect or mutate the emitter stack well enough to replace or reconfigure the rounded `CylinderLocation` style spawn shape into a box/rectangular shape.

Observed gaps:
- `niagara.list_modules` is schema-described as "List script modules of a Niagara emitter", but the plugin implementation searches for `UNiagaraScript` assets under the provided path and does not inspect the module stack for the requested system/emitter. In the Kale repro, calls against the system and `NE_Thrusters` returned `count:0`.
- `niagara.list_module_inputs` currently returns only the note `Module input enumeration requires NiagaraScript::GetInputParameters` with an empty input array. It does not identify modules such as Cylinder Location, Initialize Particle, Scale Sprite Size, custom `NMS_GlobalScale`, or their editable inputs.
- `niagara.set_module_input` currently returns only the note `Module input mutation requires NiagaraEditorModule::SetParameterOverride` and performs no mutation, save, compile, or readback.
- `niagara.list_static_switches` and `niagara.set_static_switch` are also placeholder note responses, so module variants driven by static switches cannot be inspected or changed.
- UE Python was not enough as a workaround in Kale: `UNiagaraSystem.EmitterHandles`/`SystemSpawnScript` are protected from Python, `UNiagaraSystem` C++ methods are not exposed to `call_method`, and deprecated `UNiagaraEmitter` fields such as `spawn_script_props`/`graph_source` raise Python wrapper deprecation exceptions instead of usable stack access. This needs a C++ editor-side implementation.

Expected behavior:
- Real `niagara.list_modules` must resolve both Niagara System paths and standalone Niagara Emitter paths, select the requested emitter by handle name/id or asset name, and return the actual stack modules with stable identifiers, display names, script asset paths, stack group/category, enabled state, order, and whether the module is inherited, overridden, scratch, or system-local.
- Real `niagara.list_module_inputs` must accept a module selector (`module`, `module_id`, `display_name`, or index) and return editable input variables with type, value mode, current value, default value, linked parameter, dynamic input metadata, static-switch state, and read-only/inherited diagnostics.
- Real `niagara.set_module_input` must support dry-run/validate-only/save/compile/readback and typed values for float, bool, int, enum, `FVector2f`, `FVector3f`, `FVector`, color, object/class/asset refs, and Niagara parameter links.
- Add or expose stack mutation support needed for shape replacement, not only input override: `niagara.add_module`, `niagara.remove_module`, and either `niagara.replace_module` or a safe remove+add workflow. This is required when a dogfood task needs `CylinderLocation` replaced with `BoxLocation` rather than only changing radius/height values.

Acceptance criteria:
- Against Kale `NS_RocketExhaust_Blue_Rect`, `niagara.list_modules` returns the actual modules for `NE_Thrusters`, `NE_EnergyCore`, `NE_Particulates`, and `NE_Smoke`; it must show the shape/spawn modules that make the plume round.
- `niagara.list_module_inputs` on the shape module returns enough input data to distinguish cylinder/radius/height style settings from box/dimension style settings.
- A non-dry `niagara.set_module_input` or replace-module flow can save a duplicate Niagara asset so the plume spawn footprint becomes rectangular without relying on component transform scale.
- Mutations use GameThread, `FScopedTransaction`, dirty only on real mutation, support `dry_run`/`validate_only`, and return before/after/readback plus compile diagnostics.
- Server schema, plugin handler, and tests are aligned in `server/src/tools/phase4_schemas.cpp`, `plugin/Source/SageBridge/Private/Tools/SageNiagaraTools.cpp`, and `tests/unit/test_tool_registry.cpp`; no placeholder "requires NiagaraEditorModule" success responses remain for advertised writer tools.

Implementation notes:
- The plugin likely needs Niagara editor/view-model APIs rather than Python-only reflection: resolve `FNiagaraEmitterHandle`/`FVersionedNiagaraEmitter`, build or access the stack model, enumerate `UNiagaraStackModuleItem` / `UNiagaraStackFunctionInput`, and use the Niagara editor utilities that set parameter overrides and trigger script/system compile.
- Keep a clear distinction between system-local emitter instances and shared standalone emitter assets. Default writes should affect only the requested asset/handle; shared emitter asset mutation should require an explicit confirmation flag.

### Sage Gap 2026-06-03 Niagara renderer and emitter variant read/write parity

Status: FIXED IN SOURCE - BUILD/PACKAGE/RUNTIME DOGFOOD PENDING

Blocked task:
- Kale dogfood needed a duplicate rectangular thruster VFX variant while preserving the original RocketThrusterExhaustFX package. Sage could duplicate `NS_RocketExhaust_Blue`, but could not safely inspect renderer entries, renderer bindings, or system emitter handles well enough to make a true asset-level variant or confirm whether edits would affect only the duplicate system or shared emitter assets.

Observed gaps:
- `niagara.list_emitters` reflects the protected `EmitterHandles` property generically; it does not return stable, structured handle rows with emitter name, id, enabled state, source asset/local-instance information, version, or renderer/module summary.
- `niagara.add_emitter` returns only the note `Adding emitters to NiagaraSystem requires NiagaraSystemEditorData APIs; use the Niagara System editor directly or via Python scripting`.
- `niagara.set_emitter_property` returns only the note `Emitter property mutation requires NiagaraEditorModule; use editor.run_python or the Niagara editor`.
- `niagara.list_renderers` only iterates top-level UObject properties containing `Renderer`, so for a standalone emitter it returns names like `RendererBindings` and `RendererProperties` rather than actual renderer entries (`UNiagaraSpriteRendererProperties`, material, bindings, alignment/facing/sort mode, sub-image settings, etc.).
- `niagara.add_renderer` and `niagara.remove_renderer` are placeholder note responses, and `niagara.set_renderer_property` delegates to the emitter-property stub instead of editing an indexed renderer.

Expected behavior:
- `niagara.list_emitters` must return real system emitter handles with structured data and support standalone emitter paths as a first-class target.
- Add a safe variant workflow for systems that reference shared emitters: duplicate/rebind emitter handles or "make emitter local/unique" for a duplicated Niagara System, with before/after dependency readback. This prevents a dogfood task from accidentally changing every `NS_RocketExhaust_*` system that shares `NE_Thrusters`.
- `niagara.list_renderers` must enumerate actual renderer objects by index/class/display name and return reflected editable properties, current material refs, sprite size/rotation/color/pivot/facing/alignment bindings, and whether each property is writable.
- `niagara.set_renderer_property` must write one indexed renderer property with typed coercion, dry-run/validate-only/save/compile/readback, and clear failure diagnostics when a property is not editable.
- `niagara.add_renderer` / `niagara.remove_renderer` should become real editor-side operations or be removed/renamed from the public schema until implemented; tools/list should not make stubbed renderer CRUD look production-ready.

Acceptance criteria:
- Against Kale `/FlightCore/RocketThrusterExhaustFX/FX/Emitters/NE_Thrusters.NE_Thrusters`, `niagara.list_renderers` returns at least the sprite renderer object(s), material refs, and sprite-size/color-related bindings instead of only `RendererProperties`.
- A renderer property write can be dry-run validated, then applied and saved with readback that proves the indexed renderer changed.
- A duplicated system variant can be made independent enough for rectangular RocketExhaust work without mutating the original `NS_RocketExhaust_Blue` or all shared package emitters unless the request explicitly confirms shared emitter mutation.
- Tool responses include enough dependency/ownership diagnostics for Codex to explain whether an edit is system-local, emitter-asset-wide, or renderer-only.

### Sage Gap 2026-05-30 Animation retargeting parity backlog

Status: IMPLEMENTED IN SOURCE/PACKAGE - RUNTIME DOGFOOD PENDING; KALE DEPLOYED 2026-05-31

Blocked task:
- Source gap was closed in the Sage plugin/server implementation and the packaged plugin was deployed to `D:\GameDev\Kale` with matching DLL SHA256 on 2026-05-31. Runtime editor dogfood is still pending; do not mark this as production-proven until the round-trip scenario below is run against a live editor/project.

Observed gaps:
- `animation.create_ik_rig` schema/handler now accept mesh-oriented inputs plus root/chains/dry-run/save/readback.
- `animation.read_ik_rig` now uses `UIKRigController` readback for mesh, root, chains, goals, solvers, excluded bones, and validation state.
- IK Rig retarget chain authoring is implemented: add/remove/rename chain, set start/end/goal, set root, auto-generate retarget definition, and auto-generate FBIK.
- Retargeter op-stack authoring is implemented: setup ops, add/remove/move/enable op, standalone auto-map, chain reset, FK setting write, FK/IK settings readback, and expanded pose operations.
- FBX/import/discovery helpers are implemented: single FBX animation import alias, batch FBX import, animation discovery, explicit save, root-motion batch options, and copy-bone-track repair.
- Skeleton-owned BlendMask creation/readback is implemented for UpperBody/LowerBody mask workflows via `animation.create_blend_mask` and `animation.read_blend_profiles`.
- Retarget diagnostics are implemented: inspect animation metadata, compare bones, sample bone tracks, and diagnose root-motion/missing-track/pop/quaternion-flip issues.
- Runtime retarget graph/profile support is implemented for `Retarget Pose From Mesh`: add/read/set node, assign `IKRetargeterAsset`, source mode/pin/LOD/warnings, and read/write/copy `FRetargetProfile` overrides.

Target function list:

Skeleton / ref pose / discovery:
- [x] `animation.inspect_skeleton` or a fully equivalent `animation.get_skeleton_info` parity mode: skeleton or skeletal mesh path in, full hierarchy/ref skeleton out.
- [x] `animation.inspect_ref_pose`: per-bone ref-pose transform with position, rotation quaternion/euler, parent index, optional bone filter.
- [x] `animation.list_skeletons`: content-folder skeleton discovery with recursion/filtering.
- [x] `animation.create_blend_mask`: create/update skeleton-owned BlendMask profiles such as `UpperBodyMask` / `LowerBodyMask`.
- [x] `animation.read_blend_profiles`: read skeleton-owned BlendProfile / BlendMask entries and per-bone scales.
- [x] `animation.add_skeleton_bone`: safe reference-skeleton bone add with parent/transform validation, transaction, save/readback.
- [x] `animation.copy_bone_tracks`: copy selected raw bone animation tracks from source anim to target anim with skeleton/bone compatibility diagnostics.

IK Rig authoring:
- [x] Fix `animation.create_ik_rig` schema and implementation to accept `skeletal_mesh`/`skeletal_mesh_path`, optional `retarget_root`, optional `chains[]`, `dry_run`, `save`, and readback.
- [x] Replace `animation.read_ik_rig` placeholder with real `UIKRigController` readback: skeletal mesh, retarget root, chains, goals, solvers, excluded bones, bone settings summary, warnings.
- [x] `animation.add_ik_retarget_chain`: wraps `UIKRigController::AddRetargetChain`.
- [x] `animation.remove_ik_retarget_chain`: wraps `UIKRigController::RemoveRetargetChain`.
- [x] `animation.rename_ik_retarget_chain`: wraps `UIKRigController::RenameRetargetChain`.
- [x] `animation.set_ik_retarget_chain_bones`: wraps start/end setters and validates chain coverage.
- [x] `animation.set_ik_retarget_chain_goal`: wraps `SetRetargetChainGoal`.
- [x] `animation.set_ik_retarget_root`: wraps `SetRetargetRoot`.
- [x] `animation.auto_generate_ik_retarget_definition`: wraps `ApplyAutoGeneratedRetargetDefinition` / `AutoGenerateRetargetDefinition` and returns generated chains/root/results.
- [x] `animation.auto_generate_ik_fbik`: wraps `ApplyAutoFBIK` / `AutoGenerateFBIK` and returns solver/goal changes.

IK Retargeter authoring:
- [x] Keep and harden `animation.create_ik_retargeter` with post-create `AddDefaultOps`, `AssignIKRigToAllOps`, `AutoMapChains`, `CleanAsset`, save/readback options.
- [x] Keep and harden `animation.read_ik_retargeter` to include op-stack details, op names/types/enabled state, parent relationships, chain mappings, FK settings, IK settings, profiles, and pose data.
- [x] Keep and harden `animation.set_ik_retargeter_rigs` with standalone `assign_ops`, `clean_asset`, `auto_map`, and full before/after readback.
- [x] `animation.setup_ik_retargeter_ops`: add default ops, remove duplicate ops if present, assign source/target IK rigs to all ops, auto-map chains, clean asset.
- [x] `animation.add_ik_retargeter_op`: wraps `UIKRetargeterController::AddRetargetOp`.
- [x] `animation.remove_ik_retargeter_op`: wraps `RemoveRetargetOp` with child-op diagnostics.
- [x] `animation.move_ik_retargeter_op`: wraps `MoveRetargetOpInStack`.
- [x] `animation.set_ik_retargeter_op_enabled`: wraps `SetRetargetOpEnabled`.
- [x] `animation.auto_map_ik_retargeter_chains`: standalone wrapper over `AutoMapChains` with `Exact`/`Fuzzy`/`Clear`, `force_remap`, and optional `op_name`.
- [x] Keep and harden `animation.set_ik_retargeter_chain_mapping`: map source chain to target chain and verify through `GetChainMapping`/`GetSourceChain`.
- [x] `animation.reset_ik_retargeter_chain_settings`: wraps `ResetChainSettingsToDefault` / `ResetChainSettingsInAllOps`.
- [x] `animation.set_ik_retargeter_fk_chain_settings`: configure FK chain translation/rotation modes, alpha, enable flag, especially pelvis/hips/root `GloballyScaled` safety.
- [x] Keep and expand `animation.set_ik_retargeter_pose`: add remove/duplicate/rename/reset pose support, auto-align all/bones, snap bone to ground, root offset, bone rotation offsets, and readback.

FBX import / batch retarget workflow:
- [x] `animation.import_fbx_animation`: import one FBX animation onto a skeleton with sample-rate and snap-to-frame options.
- [x] `animation.batch_import_fbx_animations`: import all matching FBX files from a directory with per-file diagnostics.
- [x] `animation.find_animations`: list animation assets in a folder with recursion/class filters.
- [x] `animation.save_animation_asset`: explicit save wrapper for retarget-created or repaired assets.
- [x] Keep and harden `animation.retarget_animations`: keep dry-run/conflict reporting, add parity with `DuplicateAndRetarget` where useful, root-motion auto-enable pattern, destination move/readback, and created-asset validation.
- [x] Keep and harden `animation.set_root_motion`: support one asset and batch paths, root-lock mode, force-root-lock options where UE exposes them.

Inspection / diagnostics:
- [x] `animation.inspect_animation`: metadata for sequence length, frames, sample rate, skeleton, root motion, additive flags, curve/track counts.
- [x] `animation.compare_retarget_bones`: compare source/target bone presence and reference-pose transform deltas.
- [x] `animation.sample_bone_tracks`: raw per-frame bone transform samples for selected bones/frames.
- [x] `animation.diagnose_retarget_animation`: automated health check for root-motion mismatch, position pops, quaternion flips, missing tracks, unmapped chains, and suspicious scale.

Runtime retargeting / AnimGraph profile:
- [x] `animation.add_retarget_pose_from_mesh_node`: author `UAnimGraphNode_RetargetPoseFromMesh` in an AnimBlueprint graph.
- [x] `animation.set_retarget_pose_from_mesh_node`: assign `IKRetargeterAsset`, source mode, exposed source mesh pin, LOD thresholds, IK LOD threshold, suppress-warnings flag.
- [x] `animation.read_retarget_pose_from_mesh_node`: return node asset refs, source mode, pins, LOD settings, and compile warnings.
- [x] `animation.read_retarget_profile`: read `FRetargetProfile`, pose overrides, force-IK-off, and op profiles.
- [x] `animation.set_retarget_profile`: write profile overrides using UE 5.7 op-profile API instead of deprecated global/root/chain settings where possible.
- [x] `animation.copy_retarget_profile_from_asset`: expose `URetargetProfileLibrary::CopyRetargetProfileFromRetargetAsset` behavior for runtime/profile authoring.

Acceptance criteria:
- [x] `tools/list` exposes valid JSON schemas for all new/changed tools; registry-level tests cover schema validity and required/optional fields.
- [x] Existing misleading contracts are fixed without breaking old clients silently: legacy args are accepted where practical and reported in readback as aliases/deprecated.
- [x] All Unreal editor mutations run on the GameThread through the existing `GT(...)`/`RunOnGameThread` path, use `FScopedTransaction`, mark packages dirty only after real mutation, and support `dry_run`/`validate_only` for risky writes.
- [x] Every writer returns structured before/after or authoritative readback plus `skipped`/`warnings`/`failures`; no silent partial success.
- [ ] Round-trip dogfood proves: create IK Rig from skeletal mesh + root + chains, read it back, create Retargeter, setup ops, auto-map chains, configure FK pelvis/hips/root, retarget at least one animation, inspect result, and run diagnostics.
- [ ] Deployed DLL hash verification before calling this production-closed.

Verification completed:
- `scripts\build-server.ps1 debug -Target sage-tests` rebuilt `sage-tests`.
- `ctest --test-dir build\debug --output-on-failure` passed 37/37.
- `scripts\build-server.ps1 debug -Target sage-server` rebuilt `sage-server.exe`.
- `scripts\audit-tools.ps1` reports 640 server tools, 622 plugin handlers, zero plugin-without-schema, zero schema/plugin stubs, and only expected local `jobs.*` schema-only tools.
- `scripts\build-plugin.ps1` packaged the Win64 plugin successfully under `build\plugin`.

Remaining verification before production-close:
- Live editor round-trip dogfood using a real skeletal mesh/source-target pair.
- Restart/reload the Kale editor as needed so the deployed `SageBridge` DLL is loaded, then run the live retarget round-trip.
