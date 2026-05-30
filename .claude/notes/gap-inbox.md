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

### Sage Gap 2026-05-30 Animation retargeting parity backlog

Status: IMPLEMENTED IN SOURCE/PACKAGE - RUNTIME DOGFOOD + DEPLOY PENDING

Blocked task:
- Source gap was closed in the Sage plugin/server implementation. Runtime editor dogfood and downstream project deployment are still pending; do not mark this as production-proven until the round-trip scenario below is run against a live editor/project.

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
- Deploy packaged `SageBridge` to target project(s), restart/reload editor as needed, and hash-check the deployed `UnrealEditor-SageBridge.dll`.

### Sage Gap 2026-05-30 Control Rig real authoring/readback surface

Status: OPEN

Blocked task:
- `animation.list_control_rig_variables` is currently a placeholder that only points users to `bp.list_variables`; it does not inspect a `UControlRigBlueprint`, `URigHierarchy`, controls, spaces, curves, hierarchy transforms, RigVM graphs, or preview mesh. This means Sage cannot yet honestly claim "Control Rig'i kontrol edebilirim" beyond generic Blueprint variable tooling.

Required function list:
- [ ] `controlrig.read`: resolve `UControlRigBlueprint`, return preview mesh, generated class, hierarchy counts, controls, bones, nulls, curves, connectors, sockets, and root elements.
- [ ] `controlrig.list_controls`: return control names, type, parent, shape/color metadata, visibility, initial/current local/global transforms, offset/shape transforms, and limits.
- [ ] `controlrig.set_preview_mesh`: wrap `UControlRigBlueprintEditorLibrary::SetPreviewMesh` with save/readback.
- [ ] `controlrig.set_control_transform`: edit hierarchy control transforms through `URigHierarchy` / controller API with dry-run, transaction, compile/save, and readback.
- [ ] `controlrig.add_control` / `controlrig.remove_control`: safe hierarchy mutation via `URigHierarchyController`, with validation and before/after dump.
- [ ] RigVM graph read/write follow-up: node list, pins, links, unit node add/remove, function library calls, compile diagnostics.

Acceptance criteria:
- No placeholder "use bp.list_variables" response remains for Control Rig-specific tools.
- Tool schemas and handlers are paired; `scripts\audit-tools.ps1` shows no schema/handler mismatch.
- UE BuildPlugin proves ControlRig/ControlRigDeveloper/ControlRigEditor module dependencies are correct.
- Live dogfood on a Control Rig asset proves read controls, set one control transform, compile/save, and read back the changed transform.
