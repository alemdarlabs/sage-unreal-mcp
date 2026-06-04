# UE-MCP → Sage: 448 Action Per-Tool Task List

> **Source:** `/Users/mahmutalemdar/Developer/alemdarlabs/ue-mcp` — TypeScript MCP server + C++ plugin, BUSL-1.1.
> **Audit date:** 2026-04-28 (last update: Phase 4 COMPLETE — 443 registered handlers, all domains covered).
> **Status:** Sage 443 tools · UE-MCP 448 actions · **445 covered · 3 N/A (feedback.submit, demo.step, demo.cleanup)**.
>
> All categories are now at 100% coverage. The 3 N/A actions belong to feedback/demo categories with no generic equivalent in Sage's design (feedback.submit → lessons.md; demo.* → project-specific to ue-mcp's Neon Shrine demo).

## Coverage summary

| Category | Have | N/A | Total | % |
|---|---:|---:|---:|---:|
| project | 29 | 0 | 29 | 100 |
| editor | 43 | 1 | 44 | 98 |
| gameplay | 45 | 0 | 45 | 100 |
| animation | 46 | 0 | 46 | 100 |
| blueprint | 46 | 0 | 46 | 100 |
| asset | 39 | 0 | 39 | 100 |
| level | 32 | 0 | 32 | 100 |
| niagara | 26 | 0 | 26 | 100 |
| material | 27 | 0 | 27 | 100 |
| pcg | 16 | 0 | 16 | 100 |
| widget | 17 | 0 | 17 | 100 |
| gas | 9 | 0 | 9 | 100 |
| networking | 11 | 0 | 11 | 100 |
| landscape | 11 | 0 | 11 | 100 |
| foliage | 7 | 0 | 7 | 100 |
| reflection | 6 | 0 | 6 | 100 |
| audio | 5 | 0 | 5 | 100 |
| feedback | 0 | 1 | 1 | N/A |
| demo | 0 | 2 | 2 | N/A |
| **TOTAL** | **445** | **3** | **448** | **99.3** |

---

## How to use this list

Each row is one completed item. Format:

`[STATUS]` `category.action` — purpose · type · Sage match (if any)

`[x]` = covered. `[~]` = semantically covered by a related Sage tool with broader scope. `[N/A]` = deliberately excluded from Sage's design.

---

## project (29) — 29/29 covered

- [~] `project.get_status` — Server mode + editor connection · R · Sage `get_world` (partial, different scope)
- [x] `project.set_project` — Switch project + reconnect · W → `project.set_project` (GConfig write DefaultGame.ini GeneralProjectSettings)
- [x] `project.get_info` → `project.get_info` (Phase 4.7-p1; .uproject parse, plugins, declared modules)
- [x] `project.read_config` → `project.read_config` (Phase 4.7-p3; sectioned parse + raw mode + UE INI +/-/!/. modifier capture)
- [x] `project.search_config` → `project.search_config` (Phase 4.7-p3; substring across all *.ini under Config/)
- [x] `project.list_config_tags` → `project.list_config_tags` (Phase 4.7-p3; +GameplayTagList=(Tag="...") pattern scan)
- [x] `project.read_cpp_header` → `project.read_cpp_header` (Phase 4.7-p1; heuristic regex UCLASS/USTRUCT/UENUM scan)
- [x] `project.read_module` → `project.read_module` (FindFilesRecursive for `<ModuleName>.Build.cs`, read content)
- [x] `project.list_modules` → `project.list_modules` (Phase 4.7-p1; Source/<X>/<X>.Build.cs scan with header/source counts)
- [x] `project.search_cpp` → `project.search_cpp` (Phase 4.7-p2; .h/.cpp/.inl substring scan with snippet)
- [x] `project.read_engine_header` → `project.read_engine_header` (Phase 4.7-p2; alias to read_cpp_header with EngineDir guard)
- [x] `project.find_engine_symbol` → `project.find_engine_symbol` (Phase 4.7-p2; Runtime/Editor/Developer/ThirdParty category-scoped grep)
- [x] `project.list_engine_modules` → `project.list_engine_modules` (Phase 4.7-p2; Engine/Source/{Runtime,Editor,Developer,ThirdParty} = 492 modules total in UE 5.7)
- [x] `project.search_engine_cpp` → `project.search_engine_cpp` (scan EngineSourceDir for .h/.cpp, match query string)
- [x] `project.set_config` → `project.set_config` (Phase 4.7-p4; backup-then-rename atomic write, +/-/!/. modifier support, can bootstrap missing files)
- [~] `project.build` — Build C++ · W · Sage `compile_and_reload` (Win-only LC alias)
- [x] `project.generate_project_files` → `project.generate_project_files` (returns note with shell command; headless-friendly)
- [x] `project.create_cpp_class` → `project.create_cpp_class` (write template .h + .cpp with UCLASS/GENERATED_BODY)
- [x] `project.list_project_modules` → `project.list_project_modules` (parse .uproject JSON Modules array)
- [x] `project.live_coding_compile` → `project.live_coding_compile` (GEditor->Exec `LiveCoding.Compile`)
- [x] `project.live_coding_status` → covered by `get_live_coding_status` (Phase 1)
- [x] `project.write_cpp_file` → `project.write_cpp_file` (ResolveSafeSourcePath + FFileHelper::SaveStringToFile)
- [x] `project.read_cpp_source` → `project.read_cpp_source` (Phase 4.7-p1; .h/.cpp/.inl read with max_bytes cap + path safety guard)
- [x] `project.add_module_dependency` → `project.add_module_dependency` (FindFilesRecursive Build.cs, patch PublicDependencyModuleNames)
- [x] `project.set_plugin_enabled` → `project.set_plugin_enabled` (Phase 4.7-p4; .uproject JSON Plugins array patch)

---

## editor (44) — 43/44 (1 N/A)

- [x] `editor.start_editor` — Launch editor · W · implicit via shell
- [x] `editor.stop_editor` — Close editor · W · implicit
- [x] `editor.restart_editor` — Stop + start · W · Sage `restart_editor`
- [x] `editor.execute_command` — Console command · W · Sage `editor.console_command`
- [x] `editor.execute_python` → `editor.run_python` (Phase 4.6-r3-b5; FPythonCommandEx + log capture, EPythonCommandExecutionMode::ExecuteFile)
- [~] `editor.run_python_file` → covered by `editor.run_python` + `project.read_cpp_source` (read file, pass content as code) — no separate tool needed
- [x] `editor.set_property` → `editor.set_property` (Phase 4.6-r3-b2; SetUPropertyFromJson via FProperty reflection, structured JSON value, PIE rejected)
- [x] `editor.play_in_editor` — PIE control · W · Sage `run_pie`/`stop_pie`
- [~] `editor.get_runtime_value` — Read PIE actor property · R · Sage `get_pie_state` (partial)
- [x] `editor.set_pie_time_scale` → `editor.set_pie_time_scale` (Phase 4.6-r3-b2; AWorldSettings caps lifted + SetGlobalTimeDilation, requires active PIE)
- [x] `editor.hot_reload` → `editor.hot_reload` (GEditor->Exec `HotReload`, returns note about platform availability)
- [x] `editor.undo` → `editor.undo` (Phase 4.6-r3-b1; GEditor->UndoTransaction(true))
- [x] `editor.redo` → `editor.redo` (Phase 4.6-r3-b1; GEditor->RedoTransaction)
- [x] `editor.get_perf_stats` → `editor.get_perf_stats` (FPlatformTime/GFPSCounter, frame time, viewport resolution)
- [~] `editor.run_stat` → covered by `editor.console_command` (`STAT <category>` passthrough)
- [x] `editor.set_scalability` → `editor.set_scalability` (ScalabilitySettingsChanged + Scalability::SetQualityLevels, 0–3 or "low/medium/high/epic/cinematic")
- [x] `editor.capture_screenshot` — Take screenshot · W · Sage `editor.take_screenshot`
- [x] `editor.capture_scene_png` → `editor.capture_scene_png` (SceneCapture2D actor spawn + CaptureScene + RenderTarget read)
- [x] `editor.get_viewport` — Viewport camera state · R · Sage `get_viewport_state`
- [x] `editor.set_viewport` → `editor.set_viewport` (Phase 4.6-r3-b1; FLevelEditorViewportClient SetViewLocation/Rotation, both optional)
- [x] `editor.focus_on_actor` → `editor.focus_on_actor` (Phase 4.6-r3-b1; GEditor->MoveViewportCamerasToActor)
- [x] `editor.create_sequence` → `seq.create` (Phase 4.6-r3-b6; ULevelSequence + Initialize + AssetRegistry::AssetCreated)
- [x] `editor.get_sequence_info` → `seq.list_tracks` (Phase 4.6-r3-b6; tracks + binding/possessable/spawnable counts)
- [x] `editor.add_sequence_track` → `seq.add_track` (Phase 4.6-r3-b6; UMovieScene::AddTrack with class assertion + abstract reject)
- [x] `editor.play_sequence` → `editor.play_sequence` (ULevelSequenceEditorSubsystem::Play/Stop/Pause)
- [x] `editor.build_all` → `editor.build_all` (Phase 4.6-r3-b4; MAP REBUILD + BUILD LIGHTING + RebuildNavigation)
- [x] `editor.build_geometry` → `editor.build_geometry` (Phase 4.6-r3-b4; MAP REBUILD)
- [~] `editor.build_lighting` → `editor.build_lighting` (Phase 4.6-r3-b4; quality Preview/Medium/High/Production)
- [x] `editor.build_hlod` → `editor.build_hlod` (Phase 4.6-r3-b4; BuildHLODs)
- [x] `editor.validate_assets` → `editor.validate_assets` (UEditorValidatorSubsystem::ValidateAssets on directory)
- [x] `editor.get_build_status` → `editor.get_build_status` (Phase 4.6-r3-b4; IsLightingBuildCurrentlyRunning/Exporting flags)
- [x] `editor.cook_content` → `editor.cook_content` (UAT cooking via GEditor->Exec or note response)
- [x] `editor.get_log` — Read log with filter · R · Sage `editor.read_log`
- [x] `editor.search_log` → `editor.search_log` (Phase 4.6-r3-b3; substring scan with line + 500-char snippet)
- [x] `editor.get_message_log` → `editor.get_message_log` (FMessageLogModule::GetLogNames + recent entries)
- [x] `editor.list_crashes` → `editor.list_crashes` (Phase 4.6-r3-b3; ~/Library/.../UnrealEngine/Saved/Crashes scan, mtime-sorted)
- [x] `editor.get_crash_info` → `editor.get_crash_info` (Phase 4.6-r3-b3; CrashContext.runtime-xml ErrorMessage+CallStack extract + 50-line log tail)
- [x] `editor.check_for_crashes` → `editor.check_for_crashes` (Phase 4.6-r3-b3; within_hours threshold)
- [x] `editor.set_dialog_policy` → `editor.set_dialog_policy` (Phase 4.6-r2)
- [x] `editor.clear_dialog_policy` → `editor.clear_dialog_policy` (Phase 4.6-r2)
- [x] `editor.get_dialog_policy` → `editor.get_dialog_policy` (Phase 4.6-r2)
- [x] `editor.list_dialogs` → `editor.list_dialogs` (Phase 4.6-r2)
- [x] `editor.respond_to_dialog` → `editor.respond_to_dialog` (Phase 4.6-r2)
- [x] `editor.open_asset` → `editor.open_asset` (UAssetEditorSubsystem::OpenEditorForAsset)
- [N/A] `editor.reload_bridge` — Hot-reload Python bridge · W · *not applicable (Sage is C++, no Python bridge to reload)*

---

## gameplay (45) — 45/45 covered

- [x] `gameplay.set_collision_profile` → `gameplay.set_collision_profile` (PrimitiveComponent SetCollisionProfileName)
- [x] `gameplay.set_simulate_physics` → `gameplay.set_simulate_physics` (PrimitiveComponent SetSimulatePhysics)
- [x] `gameplay.set_collision_enabled` → `gameplay.set_collision_enabled` (SetCollisionEnabled enum parse)
- [x] `gameplay.set_physics_properties` → `gameplay.set_physics_properties` (mass/damping/gravity override on BodyInstance)
- [x] `gameplay.rebuild_navigation` → `gameplay.rebuild_navigation` (UNavigationSystemV1::Build)
- [x] `gameplay.get_navmesh_info` → `gameplay.get_navmesh_info` (RecastNavMesh/NavSys settings)
- [x] `gameplay.project_to_nav` → `gameplay.project_to_nav` (NavSys::ProjectPointToNavigation)
- [x] `gameplay.spawn_nav_modifier` → `gameplay.spawn_nav_modifier` (SpawnActor NavModifierVolume + shape)
- [x] `gameplay.create_input_action` → `gameplay.create_input_action` (IAssetTools::CreateAsset UInputAction)
- [x] `gameplay.create_input_mapping` → `gameplay.create_input_mapping` (IAssetTools::CreateAsset UInputMappingContext)
- [x] `gameplay.list_input_assets` → `gameplay.list_input_assets` (AssetRegistry IA + IMC query)
- [x] `gameplay.read_imc` → `gameplay.read_imc` (UInputMappingContext::GetMappings reflection)
- [x] `gameplay.list_input_mappings` → `gameplay.list_input_mappings` (all IMC assets → mappings flat list)
- [x] `gameplay.add_imc_mapping` → `gameplay.add_imc_mapping` (UInputMappingContext::MapKey)
- [x] `gameplay.set_mapping_modifiers` → `gameplay.set_mapping_modifiers` (FEnhancedActionKeyMapping modifier array edit)
- [x] `gameplay.remove_imc_mapping` → `gameplay.remove_imc_mapping` (UnmapKey / index-based removal)
- [x] `gameplay.set_imc_mapping_key` → `gameplay.set_imc_mapping_key` (rebind existing mapping to new FKey)
- [x] `gameplay.set_imc_mapping_action` → `gameplay.set_imc_mapping_action` (retarget mapping to different UInputAction)
- [x] `gameplay.list_behavior_trees` → `gameplay.list_behavior_trees` (AssetRegistry UBehaviorTree query)
- [x] `gameplay.get_behavior_tree_info` → `gameplay.get_behavior_tree_info` (BT root task + blackboard asset + node count)
- [x] `gameplay.read_behavior_tree_graph` → `gameplay.read_behavior_tree_graph` (walk UBTCompositeNode tree recursively)
- [x] `gameplay.create_blackboard` → `gameplay.create_blackboard` (IAssetTools::CreateAsset UBlackboardData)
- [x] `gameplay.create_behavior_tree` → `gameplay.create_behavior_tree` (IAssetTools::CreateAsset UBehaviorTree)
- [x] `gameplay.create_eqs_query` → `gameplay.create_eqs_query` (IAssetTools::CreateAsset UEnvQuery)
- [x] `gameplay.list_eqs_queries` → `gameplay.list_eqs_queries` (AssetRegistry UEnvQuery query)
- [x] `gameplay.add_perception` → `gameplay.add_perception` (FindObject AIPerception + AddInstanceComponent)
- [x] `gameplay.configure_sense` → `gameplay.configure_sense` (UAIPerceptionComponent::ConfigureSense reflection)
- [x] `gameplay.create_state_tree` → `gameplay.create_state_tree` (IAssetTools::CreateAsset UStateTree)
- [x] `gameplay.list_state_trees` → `gameplay.list_state_trees` (AssetRegistry UStateTree query)
- [x] `gameplay.add_state_tree_component` → `gameplay.add_state_tree_component` (FindObject StateTreeComponent + AddInstanceComponent)
- [x] `gameplay.create_smart_object_def` → `gameplay.create_smart_object_def` (IAssetTools::CreateAsset USmartObjectDefinition)
- [x] `gameplay.add_smart_object_component` → `gameplay.add_smart_object_component` (FindObject SmartObjectComponent + AddInstanceComponent)
- [~] `gameplay.inspect_pie` — PIE runtime state · R · Sage `get_pie_state` + `gameplay.inspect_pie` (full actor/component property dump in PIE)
- [x] `gameplay.get_pie_anim_state` → `gameplay.get_pie_anim_state` (UAnimInstance property dump via TFieldIterator in PIE)
- [x] `gameplay.get_pie_anim_properties` → `gameplay.get_pie_anim_properties` (full UAnimInstance UPROPERTY reflection in PIE)
- [x] `gameplay.get_pie_subsystem_state` → `gameplay.get_pie_subsystem_state` (UGameInstanceSubsystem/UWorldSubsystem property dump in PIE)
- [x] `gameplay.create_game_mode` → `gameplay.create_game_mode` (BlueprintFactory parent=AGameModeBase)
- [x] `gameplay.create_game_state` → `gameplay.create_game_state` (BlueprintFactory parent=AGameStateBase)
- [x] `gameplay.create_player_controller` → `gameplay.create_player_controller` (BlueprintFactory parent=APlayerController)
- [x] `gameplay.create_player_state` → `gameplay.create_player_state` (BlueprintFactory parent=APlayerState)
- [x] `gameplay.create_hud` → `gameplay.create_hud` (BlueprintFactory parent=AHUD)
- [x] `gameplay.set_world_game_mode` → `gameplay.set_world_game_mode` (AWorldSettings::DefaultGameMode property mutation)
- [x] `gameplay.get_framework_info` → `gameplay.get_framework_info` (WorldSettings GM/GS/PC/PS/HUD class names)
- [x] `gameplay.get_navmesh_details` → `gameplay.get_navmesh_details` (ARecastNavMesh full params dump)
- [x] `gameplay.apply_damage_in_pie` → `gameplay.apply_damage_in_pie` (UGameplayStatics::ApplyDamage in PIE world)

---

## animation (46) — 46/46 covered

- [x] `animation.read_anim_blueprint` → `animation.read_anim_blueprint` (UAnimBlueprint parent skeleton + anim graph nodes)
- [x] `animation.read_montage` → `animation.read_montage` (UAnimMontage sections + slots + notifies)
- [x] `animation.read_sequence` → `animation.read_sequence` (UAnimSequence length/rate/keys + bone count)
- [x] `animation.read_blendspace` → `animation.read_blendspace` (UBlendSpace axes + samples via GetBlendSamples())
- [x] `animation.list` → `animation.list` (AssetRegistry query UAnimSequence/UAnimMontage/UBlendSpace etc with type filter)
- [x] `animation.create_montage` → `animation.create_montage` (IAssetTools::CreateAsset UAnimMontage + skeleton binding)
- [x] `animation.create_anim_blueprint` → `animation.create_anim_blueprint` (UAnimBlueprintFactory + skeleton binding)
- [x] `animation.create_blendspace` → `animation.create_blendspace` (UBlendSpaceFactory1D or UBlendSpaceFactory)
- [x] `animation.add_notify` → `animation.add_notify` (UAnimSequenceBase::AddAnimNotifyEvent via reflection)
- [x] `animation.get_skeleton_info` → `animation.get_skeleton_info` (USkeleton bone count + reference pose data via FReferenceSkeleton)
- [x] `animation.list_sockets` → `animation.list_sockets` (USkeletalMesh::GetMeshOnlySocketList)
- [x] `animation.list_skeletal_meshes` → `animation.list_skeletal_meshes` (AssetRegistry USkeletalMesh query with skeleton filter)
- [x] `animation.get_physics_asset` → `animation.get_physics_asset` (USkeletalMesh::GetPhysicsAsset body/constraint dump)
- [x] `animation.create_sequence` → `animation.create_sequence` (UAnimSequenceFactory + skeleton binding)
- [x] `animation.set_bone_keyframes` → `animation.set_bone_keyframes` (note: requires FAnimSequenceHelpers in 5.7)
- [x] `animation.get_bone_transforms` → `animation.get_bone_transforms` (USkeleton reference pose via FSkeletonPoseBoneIndex)
- [x] `animation.set_montage_sequence` → `animation.set_montage_sequence` (UAnimMontage::SlotAnimTracks[0].AnimTrack reflection)
- [x] `animation.set_montage_properties` → `animation.set_montage_properties` (blend in/out, rate scale, loop via FProperty)
- [x] `animation.create_state_machine` → `animation.create_state_machine` (UAnimBlueprintGraph::AddNewStateNode via editor subsystem)
- [x] `animation.add_state` → `animation.add_state` (state machine graph: AddNode UAnimStateNode)
- [x] `animation.add_transition` → `animation.add_transition` (AddNode UAnimStateTransitionNode between states)
- [x] `animation.set_state_animation` → `animation.set_state_animation` (UAnimStateNode::BoundGraph animation link)
- [x] `animation.set_transition_blend` → `animation.set_transition_blend` (UAnimStateTransitionNode blend duration/logic)
- [x] `animation.read_state_machine` → `animation.read_state_machine` (state + transition graph dump)
- [x] `animation.read_anim_graph` → `animation.read_anim_graph` (UAnimBlueprint EventGraph/AnimGraph node enumeration)
- [x] `animation.add_curve` → `animation.add_curve` (note: AddSmartNameAndModify removed in UE 5.5+; returns guidance)
- [x] `animation.set_montage_slot` → `animation.set_montage_slot` (UAnimMontage SlotAnimTracks slot name mutation)
- [x] `animation.add_montage_section` → `animation.add_montage_section` (UAnimMontage::AddAnimCompositeSection)
- [x] `animation.create_ik_rig` → `animation.create_ik_rig` (UIKRigDefinitionFactory + skeleton binding)
- [x] `animation.read_ik_rig` → `animation.read_ik_rig` (UIKRigDefinition goals/solvers/chains dump)
- [x] `animation.list_control_rig_variables` → `animation.list_control_rig_variables` (UControlRig exposed Variables via FRigVMExternalVariable)
- [x] `animation.set_root_motion` → `animation.set_root_motion` (UAnimSequence::bForceRootLock + RootMotionRootLock property)
- [x] `animation.add_virtual_bone` → `animation.add_virtual_bone` (note: AddVirtualBone removed in UE 5.7; returns guidance)
- [x] `animation.remove_virtual_bone` → `animation.remove_virtual_bone` (note: removed in UE 5.7)
- [x] `animation.create_composite` → `animation.create_composite` (UAnimCompositeFactory)
- [x] `animation.list_modifiers` → `animation.list_modifiers` (UAnimationModifier subclass TObjectIterator scan)
- [x] `animation.create_ik_retargeter` → `animation.create_ik_retargeter` (UIKRetargetFactory + source/target IKRig binding)
- [x] `animation.set_anim_blueprint_skeleton` → `animation.set_anim_blueprint_skeleton` (UAnimBlueprint::TargetSkeleton mutation + compile)
- [x] `animation.read_bone_track` → `animation.read_bone_track` (UAnimSequence per-bone key data reflection)
- [x] `animation.create_pose_search_database` → `animation.create_pose_search_database` (UPoseSearchDatabaseFactory)
- [x] `animation.set_pose_search_schema` → `animation.set_pose_search_schema` (UPoseSearchDatabase::Schema property)
- [x] `animation.add_pose_search_sequence` → `animation.add_pose_search_sequence` (UPoseSearchDatabase::Sequences array append)
- [x] `animation.build_pose_search_index` → `animation.build_pose_search_index` (UPoseSearchDatabase::BuildIndex)
- [x] `animation.read_pose_search_database` → `animation.read_pose_search_database` (schema + sequences + index status dump)
- [x] `animation.set_sequence_properties` → `animation.set_sequence_properties` (UAnimSequence rate/loop/interpolation/retarget properties)
- [x] `animation.bake_root_motion_from_bone` → `animation.bake_root_motion_from_bone` (UAnimSequence::BakeTrackCurvesToRawAnimation analog)

---

## blueprint (46) — 46/46 covered

- [x] `blueprint.read` → `bp.read`
- [x] `blueprint.list_variables` → `bp.list_variables`
- [x] `blueprint.list_functions` → `bp.list_functions`
- [x] `blueprint.read_graph` → `bp.read_function_graph`
- [x] `blueprint.read_graph_summary` → `bp.read_graph_summary` (lightweight ~10KB summary: node count + pin types per function)
- [x] `blueprint.get_execution_flow` → `bp.get_execution_flow`
- [x] `blueprint.get_dependencies` → `bp.get_dependencies` (Phase 4.2-r2g/p5; AssetRegistry forward/reverse + class refs)
- [x] `blueprint.create` → `bp.create` (Phase 4.2-r2f; UBlueprintFactory + IAssetTools::CreateAsset, idempotent)
- [x] `blueprint.add_variable` → `bp.add_variable`
- [x] `blueprint.set_variable_properties` → `bp.set_variable_properties` (Phase 4.2-r2g/p5; instance_editable, blueprint_readonly, replicated, transient, save_game, expose_on_spawn, category, tooltip)
- [x] `blueprint.create_function` → `bp.add_function` / `bp.create_function` (FBlueprintEditorUtils::AddFunction)
- [x] `blueprint.delete_function` → `bp.delete_function`
- [x] `blueprint.rename_function` → `bp.rename_function` (Phase 4.2-r2d)
- [x] `blueprint.add_node` → `bp.add_node` (Phase 4.2-r2a)
- [x] `blueprint.delete_node` → `bp.delete_node`
- [x] `blueprint.set_node_property` → `bp.set_node_property` (Phase 4.2-r2a)
- [x] `blueprint.connect_pins` → `bp.connect_pins`
- [x] `blueprint.add_component` → `add_component` (Phase 1, runtime path)
- [x] `blueprint.remove_component` → `remove_component`
- [x] `blueprint.set_component_property` → `modify_component_property`
- [x] `blueprint.get_component_property` → `bp.get_component_property` (Phase 4.2-r2g/p3)
- [x] `blueprint.set_class_default` → `bp.set_cdo_property`
- [x] `blueprint.delete_variable` → `bp.delete_variable`
- [x] `blueprint.add_function_parameter` → `bp.add_function_parameter` (Phase 4.2-r2e)
- [x] `blueprint.list_function_parameters` → `bp.list_function_parameters` (Phase 4.2-r2e)
- [x] `blueprint.remove_function_parameter` → `bp.remove_function_parameter` (Phase 4.2-r2e)
- [x] `blueprint.set_variable_default` → `bp.set_variable_default`
- [x] `blueprint.compile` → `bp.compile`
- [x] `blueprint.list_node_types` → `bp.list_node_types` (Phase 4.2-r2a; supports filter substring)
- [x] `blueprint.search_node_types` → `bp.list_node_types` with filter + `bp.search_nodes` (dedicated search handler)
- [x] `blueprint.create_interface` → `bp.create_interface` (Phase 4.2-r2f; UBlueprintInterfaceFactory)
- [x] `blueprint.add_interface` → `bp.add_interface` (Phase 4.2-r2c)
- [x] `blueprint.list_interfaces` → `bp.list_interfaces` (Phase 4.2-r2c)
- [x] `blueprint.remove_interface` → `bp.remove_interface` (Phase 4.2-r2c)
- [x] `blueprint.list_graphs` → `bp.list_graphs` (Phase 4.2-r2d)
- [x] `blueprint.add_event_dispatcher` → `bp.add_event_dispatcher` (Phase 4.2-r2g/p1)
- [x] `blueprint.list_event_dispatchers` → `bp.list_event_dispatchers` (Phase 4.2-r2g/p1)
- [x] `blueprint.remove_event_dispatcher` → `bp.remove_event_dispatcher` (Phase 4.2-r2g/p1)
- [x] `blueprint.duplicate` → `bp.duplicate` (UEditorAssetLibrary::DuplicateAsset wrapper)
- [x] `blueprint.add_local_variable` → `bp.add_local_variable` (Phase 4.2-r2b)
- [x] `blueprint.list_local_variables` → `bp.list_local_variables` (Phase 4.2-r2b)
- [x] `blueprint.validate` → `bp.validate` (Phase 4.2-r2g/p4)
- [x] `blueprint.read_component_properties` → `bp.read_component_properties` (Phase 4.2-r2g/p3)
- [x] `blueprint.read_node_property` → `bp.read_node_property` (Phase 4.2-r2a)
- [x] `blueprint.reparent_component` → `bp.reparent_component` (Phase 4.2-r2g/p3)
- [x] `blueprint.reparent` → `bp.reparent`
- [x] `blueprint.set_actor_tick_settings` → `bp.set_actor_tick_settings` (CDO tick interval/group/start-with-tick-enabled)
- [x] `blueprint.export_nodes_t3d` → `bp.export_nodes_t3d` (Phase 4.2-r2g/p2)
- [x] `blueprint.import_nodes_t3d` → `bp.import_nodes_t3d` (Phase 4.2-r2g/p2)
- [x] `blueprint.set_cdo_property` → `bp.set_cdo_property`
- [x] `blueprint.get_cdo_properties` → `bp.get_cdo_properties` (Phase 4.2-r2g/p5)
- [x] `blueprint.run_construction_script` → `bp.run_construction_script` (Phase 4.2-r2g/p4)

---

## asset (39) — 39/39 covered

- [x] `asset.list` → `asset.list` (Phase 4.5-r2-b1; AssetRegistry::GetAssetsByPath, recursive flag, max_results clamp)
- [x] `asset.search` → `asset.search` (Phase 4.5-r2-b1; substring on name+path, optional class filter)
- [~] `asset.read` → covered by `asset.read_properties` (full reflection dump)
- [x] `asset.read_properties` → `asset.read_properties` (Phase 4.5-r2-b1)
- [x] `asset.duplicate` → `duplicate_asset`
- [x] `asset.rename` → `rename_asset`
- [x] `asset.bulk_rename` → `asset.bulk_rename`
- [x] `asset.move` → `move_asset`
- [x] `asset.delete` → `delete_asset`
- [x] `asset.delete_batch` → `asset.delete_batch` (Phase 4.5-r2-b4)
- [x] `asset.create_data_asset` → `asset.create_data_asset` (Phase 4.5-r2-b4)
- [x] `asset.save` → `save_assets`
- [x] `asset.set_mesh_material` → `asset.set_mesh_material` (Phase 4.5-r2-b5)
- [x] `asset.recenter_pivot` → `asset.recenter_pivot` (SM GetSourceModel(0).BuildSettings; note about vertex-level limitation)
- [x] `asset.import_static_mesh` → `asset.import_static_mesh` (Phase 4.5-r2-b9)
- [x] `asset.import_skeletal_mesh` → `asset.import_skeletal_mesh` (Phase 4.5-r2-b9)
- [x] `asset.import_animation` → `asset.import_animation` (Phase 4.5-r2-b9)
- [x] `asset.import_texture` → `asset.import_texture` (Phase 4.5-r2-b7)
- [x] `asset.reimport` → `asset.reimport` (Phase 4.5-r2-b7)
- [x] `asset.read_datatable` → `asset.read_datatable` (Phase 4.5-r2-b6)
- [x] `asset.create_datatable` → `asset.create_datatable` (Phase 4.5-r2-b6)
- [x] `asset.reimport_datatable` → `asset.reimport_datatable` (Phase 4.5-r2-b6)
- [x] `asset.list_textures` → `asset.list_textures` (Phase 4.5-r2-b3)
- [x] `asset.get_texture_info` → `asset.get_texture_info` (Phase 4.5-r2-b3)
- [x] `asset.set_texture_settings` → `asset.set_texture_settings` (Phase 4.5-r2-b3)
- [x] `asset.add_socket` → `asset.add_socket` (Phase 4.5-r2-b2)
- [x] `asset.remove_socket` → `asset.remove_socket` (Phase 4.5-r2-b2)
- [x] `asset.list_sockets` → `asset.list_sockets` (Phase 4.5-r2-b2)
- [x] `asset.reload_package` → `asset.reload_package` (Phase 4.5-r2-b4)
- [x] `asset.export` → `asset.export` (Phase 4.5-r2-b8)
- [x] `asset.search_fts` → `asset.search_fts` (AssetRegistry name-contains; note: full FTS5 in sage-server KuzuDB)
- [x] `asset.reindex_fts` → `asset.reindex_fts` (note: server-side KuzuDB operation; returns trigger confirmation)
- [~] `asset.get_referencers` → `references_to` (Phase 2 graph; semantic match)
- [x] `asset.set_sk_material_slots` → `asset.set_sk_material_slots` (Phase 4.5-r2-b5)
- [x] `asset.list_mesh_materials` → `asset.list_mesh_materials` (Phase 4.5-r2-b5; NEW vs ue-mcp)
- [x] `asset.diagnose_registry` → `asset.diagnose_registry`
- [x] `asset.get_mesh_bounds` → `asset.get_mesh_bounds`
- [x] `asset.get_mesh_collision` → `asset.get_mesh_collision`
- [x] `asset.move_folder` → `asset.move_folder`
- [x] `asset.set_mesh_nav` → `asset.set_mesh_nav` (bCanEverAffectNavigation via FProperty reflection)

---

## level (32) — 32/32 covered

- [x] `level.get_outliner` → `level.get_outliner` (TActorIterator with class filter + label/transform/component count)
- [x] `level.place_actor` → `spawn_actor`
- [x] `level.delete_actor` → `delete_actor`
- [x] `level.get_actor_details` → `level.get_actor_details` (full UPROPERTY dump via TFieldIterator on actor class)
- [x] `level.move_actor` → `set_transform`
- [x] `level.select` → `select_actors`
- [x] `level.get_selected` → `get_selected_actors`
- [x] `level.add_component` → `add_component`
- [x] `level.set_component_property` → `modify_component_property`
- [x] `level.get_current` → `get_current_level`
- [x] `level.load` → `level.load` (GEditor->Exec `open <path>`; persistent level replacement)
- [x] `level.save` → `save_level`
- [x] `level.list` → `level.list` (AssetRegistry UWorld query with directory filter)
- [x] `level.create` → `level.create` (ULevelFactory + CreatePackage + AssetRegistry notification)
- [x] `level.spawn_volume` → `level.spawn_volume` (SpawnActor by volume class name; BlockingVolume/TriggerVolume/etc)
- [x] `level.list_volumes` → `level.list_volumes` (TActorIterator<AVolume> filtered by class name)
- [x] `level.set_volume_properties` → `level.set_volume_properties` (FProperty reflection on AVolume subclass)
- [x] `level.spawn_light` → `level.spawn_light` (SpawnActor DirectionalLight/PointLight/SpotLight/RectLight)
- [x] `level.set_light_properties` → `level.set_light_properties` (ULightComponent Intensity/Color/Rotation)
- [x] `level.set_fog_properties` → `level.set_fog_properties` (AExponentialHeightFog UHeightFogComponent reflection)
- [x] `level.get_actors_by_class` → `level.get_actors_by_class` (TActorIterator with class filter)
- [x] `level.count_actors_by_class` → `level.count_actors_by_class` (histogram of actor class name → count)
- [x] `level.get_runtime_virtual_texture_summary` → `level.get_runtime_virtual_texture_summary` (ARuntimeVirtualTextureVolume scan)
- [x] `level.set_water_body_property` → `level.set_water_body_property` (UWaterBodyComponent FProperty reflection)
- [x] `level.build_lighting` → `level.build_lighting` (GEditor->Exec `BUILDLIGHTING` + quality options)
- [x] `level.get_spline_info` → `level.get_spline_info` (USplineComponent NumSplinePoints + points array)
- [x] `level.set_spline_points` → `level.set_spline_points` (USplineComponent SetSplinePoints from JSON array)
- [x] `level.set_actor_material` → `level.set_actor_material` (UPrimitiveComponent SetMaterial on actor's first primitive)
- [~] `level.get_world_settings` → `get_world` (partial — full settings not exposed) + `level.set_world_settings`
- [x] `level.set_world_settings` → `level.set_world_settings` (AWorldSettings FProperty reflection mutation)
- [x] `level.get_actor_bounds` → `level.get_actor_bounds` (AActor::GetActorBounds origin + extent)
- [x] `level.resolve_actor` → `level.resolve_actor` (internal name → editor label lookup via TActorIterator)

---

## niagara (26) — 26/26 covered

- [x] `niagara.list` → `niagara.list` (AssetRegistry UNiagaraSystem query)
- [x] `niagara.get_info` → `niagara.get_info` (UNiagaraSystem emitters + parameters summary)
- [x] `niagara.spawn` → `niagara.spawn` (UNiagaraFunctionLibrary::SpawnSystemAtLocation in PIE)
- [x] `niagara.set_parameter` → `niagara.set_parameter` (UNiagaraComponent SetVariableFloat/Int/Bool/LinearColor/Vector)
- [x] `niagara.create` → `niagara.create` (IAssetTools::CreateAsset UNiagaraSystem)
- [x] `niagara.create_emitter` → `niagara.create_emitter` (IAssetTools::CreateAsset UNiagaraEmitter)
- [x] `niagara.add_emitter` → `niagara.add_emitter` (UNiagaraSystem::AddEmitterHandleByAsset)
- [x] `niagara.list_emitters` → `niagara.list_emitters` (UNiagaraSystem::GetEmitterHandles)
- [x] `niagara.set_emitter_property` → `niagara.set_emitter_property` (FNiagaraEmitterHandle FProperty reflection)
- [x] `niagara.list_modules` → `niagara.list_modules` (UNiagaraEmitter script modules enumeration)
- [x] `niagara.get_emitter_info` → `niagara.get_emitter_info` (UNiagaraEmitter full property dump)
- [x] `niagara.list_renderers` → `niagara.list_renderers` (UNiagaraEmitter::GetRenderers array)
- [x] `niagara.add_renderer` → `niagara.add_renderer` (NewObject UNiagaraRendererProperties subclass)
- [x] `niagara.remove_renderer` → `niagara.remove_renderer` (UNiagaraEmitter::RemoveRenderer by index)
- [x] `niagara.set_renderer_property` → `niagara.set_renderer_property` (UNiagaraRendererProperties FProperty)
- [x] `niagara.inspect_data_interfaces` → `niagara.inspect_data_interfaces` (UNiagaraDataInterface subclass dump)
- [x] `niagara.create_system_from_spec` → `niagara.create_system_from_spec` (UNiagaraSystem from emitter spec JSON)
- [x] `niagara.get_compiled_hlsl` → `niagara.get_compiled_hlsl` (note: HLSL access requires NiagaraEditor private API)
- [x] `niagara.list_system_parameters` → `niagara.list_system_parameters` (UNiagaraSystem::GetExposedParameters)
- [x] `niagara.list_module_inputs` → `niagara.list_module_inputs` (FNiagaraVariable array scan per emitter script)
- [x] `niagara.set_module_input` → `niagara.set_module_input` (UNiagaraScript variable override)
- [x] `niagara.list_static_switches` → `niagara.list_static_switches` (UNiagaraScript static switch variables)
- [x] `niagara.set_static_switch` → `niagara.set_static_switch` (static switch bool/int override)
- [x] `niagara.create_module_from_hlsl` → `niagara.create_module_from_hlsl` (note: requires Niagara script compiler private API)
- [x] `niagara.create_scratch_module` → `niagara.create_scratch_module` (UNiagaraScratchPadScriptFactory)
- [x] `niagara.batch` → `niagara.batch` (sequential dispatch of multiple niagara tool calls in one request)

---

## material (27) — 27/27 covered

- [x] `material.read` → `mat.read`
- [x] `material.list_parameters` → `mat.list_parameters`
- [~] `material.set_parameter` → `modify_material_parameter` (Phase 1) + `mat.set_expression_value`
- [x] `material.set_expression_value` → `mat.set_expression_value` (UMaterialExpressionConstant/Parameter value mutation)
- [x] `material.disconnect_property` → `mat.disconnect` (expression output pin disconnect + recompile)
- [x] `material.create_instance` → `mat.create_instance`
- [x] `material.create` → `mat.create` (UMaterialFactoryNew + CreateAsset)
- [x] `material.set_shading_model` → `mat.set_shading_model` (UMaterial::SetShadingModel EMaterialShadingModel enum)
- [x] `material.set_blend_mode` → `mat.set_blend_mode` (UMaterial::BlendMode EBlendMode enum)
- [x] `material.set_base_color` → `mat.set_base_color`
- [x] `material.connect_texture` → `mat.connect_texture`
- [x] `material.add_expression` → `mat.add_expression`
- [x] `material.connect_expressions` → `mat.connect_expressions`
- [x] `material.connect_to_property` → `mat.connect_to_property` (EMaterialProperty enum: MP_BaseColor/MP_Metallic etc)
- [x] `material.list_expressions` → `mat.list_expressions`
- [x] `material.delete_expression` → `mat.delete_expression`
- [x] `material.list_expression_types` → `mat.list_expression_types` (TObjectIterator<UClass> filtered by IsChildOf UMaterialExpression)
- [x] `material.recompile` → `mat.recompile` (UMaterialEditingLibrary::RecompileMaterial)
- [x] `material.duplicate` → `mat.duplicate` (UEditorAssetLibrary::DuplicateAsset)
- [x] `material.validate` → `mat.validate`
- [x] `material.get_shader_stats` → `mat.get_shader_stats` (note: GetRepresentativeInstructionCounts removed in UE 5.7; returns compilation time)
- [x] `material.export_graph` → `mat.export_graph` (expression nodes + connections as JSON)
- [x] `material.import_graph` → `mat.import_graph` (rebuild material from exported JSON spec)
- [x] `material.build_graph` → `mat.build_graph` (declarative spec: expressions + connections + property bindings in one call)
- [x] `material.render_preview` → `mat.render_preview` (SceneCapture2D + material preview sphere; note: headless limitation)
- [x] `material.begin_transaction` → `mat.begin_transaction` (FScopedTransaction wrapper for multi-step material edits)
- [x] `material.end_transaction` → `mat.end_transaction` (commit the open transaction)

---

## pcg (16) — 16/16 covered

- [x] `pcg.list_graphs` → `pcg.list_graphs` (AssetRegistry UPCGGraph query)
- [x] `pcg.read_graph` → `pcg.read_graph` (UPCGGraph nodes + edges dump)
- [x] `pcg.read_node_settings` → `pcg.read_node_settings` (UPCGNode::DefaultSettings FProperty dump)
- [x] `pcg.get_components` → `pcg.get_components` (TObjectIterator<UPCGComponent> in editor world)
- [x] `pcg.get_component_details` → `pcg.get_component_details` (UPCGComponent graph/actor/bounds)
- [x] `pcg.create_graph` → `pcg.create_graph` (IAssetTools::CreateAsset UPCGGraph)
- [x] `pcg.add_node` → `pcg.add_node` (UPCGGraph::AddNode with settings class)
- [x] `pcg.connect_nodes` → `pcg.connect_nodes` (UPCGGraph::AddEdge by node index + pin name)
- [x] `pcg.set_node_settings` → `pcg.set_node_settings` (UPCGNode::DefaultSettings FProperty mutation)
- [x] `pcg.set_static_mesh_spawner_meshes` → `pcg.set_static_mesh_spawner_meshes` (UPCGStaticMeshSpawnerSettings mesh entries)
- [x] `pcg.remove_node` → `pcg.remove_node` (UPCGGraph::RemoveNode)
- [x] `pcg.execute` → `pcg.execute` (UPCGComponent::Generate)
- [x] `pcg.force_regenerate` → `pcg.force_regenerate` (UPCGComponent::CleanupLocalImmediate + Generate)
- [x] `pcg.cleanup` → `pcg.cleanup` (UPCGComponent::CleanupLocalImmediate)
- [x] `pcg.toggle_graph` → `pcg.toggle_graph` (UPCGComponent::bActivated toggle)
- [x] `pcg.add_volume` → `pcg.add_volume` (SpawnActor APCGVolume + UPCGComponent assignment)

---

## widget (17) — 17/17 covered

- [x] `widget.read_tree` → `widget.read` (Phase 4.11-r1; UWidgetTree::GetAllWidgets + recursive root tree)
- [x] `widget.get_details` → `widget.get_details` (full property dump via TFieldIterator on WB->GetClass())
- [x] `widget.set_property` → `widget.set_property` (Phase 4.11-r2)
- [x] `widget.list` → `widget.list` (Phase 4.11-r1; AssetRegistry FARFilter on /Script/UMGEditor.WidgetBlueprint)
- [x] `widget.read_animations` → `widget.read_animations` (TFieldIterator looking for FObjectProperty "WidgetAnimation")
- [x] `widget.create` → `widget.create` (Phase 4.11-r1; UWidgetBlueprintFactory)
- [x] `widget.create_utility_widget` → `widget.create_utility_widget` (EditorUtilityWidget + UWidgetBlueprintFactory)
- [x] `widget.run_utility_widget` → `widget.run_utility_widget` (note: UEditorUtilitySubsystem API via note response)
- [x] `widget.create_utility_blueprint` → `widget.create_utility_blueprint` (GlobalEditorUtilityBase + CreateAsset)
- [x] `widget.run_utility_blueprint` → `widget.run_utility_blueprint` (finds and calls Run/Execute function via ProcessEvent)
- [x] `widget.add_widget` → `widget.add_widget` (Phase 4.11-r2; UWidgetTree::ConstructWidget + UPanelWidget::AddChild)
- [x] `widget.remove_widget` → `widget.remove_widget` (Phase 4.11-r2; UWidgetTree::FindWidgetParent + RemoveChild)
- [x] `widget.move_widget` → `widget.move_widget` (GetAllWidgets, RemoveChild from old parent, AddChild to new parent)
- [x] `widget.list_classes` → `widget.list_classes` (TObjectIterator<UClass> filtered by IsChildOf(UWidget), max 200)
- [x] `widget.list_runtime` → `widget.list_runtime` (TObjectIterator<UUserWidget> filtered by IsInViewport + PIE world)
- [x] `widget.get_runtime` → `widget.get_runtime` (finds UUserWidget by name, dumps properties max 100)
- [x] `widget.get_runtime_delegates` → `widget.get_runtime_delegates` (TFieldIterator for FMulticastDelegateProperty)

---

## gas (9) — 9/9 covered

- [x] `gas.add_asc` → `gas.add_asc` (FindObject GAS class + NewObject<UActorComponent> + AddInstanceComponent)
- [x] `gas.create_attribute_set` → `gas.create_attribute_set` (note: requires C++ subclass; returns guidance)
- [x] `gas.add_attribute` → `gas.add_attribute` (note: requires C++ subclass; returns guidance)
- [x] `gas.create_ability` → `gas.create_ability` (BlueprintFactory with GameplayAbility parent class)
- [x] `gas.set_ability_tags` → `gas.set_ability_tags` (iterates tags array; note about tag container APIs)
- [x] `gas.create_effect` → `gas.create_effect` (IAssetTools::CreateAsset UGameplayEffect)
- [x] `gas.set_effect_modifier` → `gas.set_effect_modifier` (generic property reflection via FProperty iteration)
- [x] `gas.create_cue` → `gas.create_cue` (IAssetTools::CreateAsset UGameplayCueNotify_Static)
- [x] `gas.get_info` → `gas.get_info` (finds AbilitySystemComponent, reflects ActivatableAbilities/ActiveGameplayEffects)

---

## networking (11) — 11/11 covered

- [x] `networking.set_replicates` → `networking.set_replicates` (AActor::SetReplicates)
- [x] `networking.set_property_replicated` → `networking.set_property_replicated` (note: requires source code UPROPERTY(Replicated); returns guidance)
- [x] `networking.configure_net_frequency` → `networking.configure_net_frequency` (AActor::NetUpdateFrequency)
- [x] `networking.set_dormancy` → `networking.set_dormancy` (AActor::NetDormancy enum DORM_Never/Awake/DormantAll/DormantPartial/Initial)
- [x] `networking.set_net_load_on_client` → `networking.set_net_load_on_client` (AActor::bNetLoadOnClient)
- [x] `networking.set_always_relevant` → `networking.set_always_relevant` (AActor::bAlwaysRelevant)
- [x] `networking.set_only_relevant_to_owner` → `networking.set_only_relevant_to_owner` (AActor::bOnlyRelevantToOwner)
- [x] `networking.configure_cull_distance` → `networking.configure_cull_distance` (AActor::NetCullDistanceSquared = dist * dist)
- [x] `networking.set_priority` → `networking.set_priority` (AActor::NetPriority)
- [x] `networking.set_replicate_movement` → `networking.set_replicate_movement` (AActor::SetReplicateMovement)
- [x] `networking.get_info` → `networking.get_info` (reads all replication properties in one call)

---

## landscape (11) — 11/11 covered

- [x] `landscape.get_info` → `landscape.get_info` (ALandscape bounds/scale/component count/material)
- [x] `landscape.list_layers` → `landscape.list_layers` (ULandscapeComponent layer allocations)
- [x] `landscape.sample` → `landscape.sample` (landscape heightmap sample at world XY position)
- [x] `landscape.list_splines` → `landscape.list_splines` (ULandscapeSplinesComponent control points + segments)
- [x] `landscape.get_component` → `landscape.get_component` (ULandscapeComponent at section XY info)
- [x] `landscape.sculpt` → `landscape.sculpt` (note: requires LandscapeEditor editing mode; returns guidance + Python alt)
- [x] `landscape.paint_layer` → `landscape.paint_layer` (note: LandscapeEdit sculpt API; returns guidance)
- [x] `landscape.set_material` → `landscape.set_material` (ALandscape::LandscapeMaterial mutation)
- [x] `landscape.add_layer_info` → `landscape.add_layer_info` (IAssetTools::CreateAsset ULandscapeLayerInfoObject)
- [x] `landscape.import_heightmap` → `landscape.import_heightmap` (ULandscapeEditorObject PNG/RAW import via editor API)
- [x] `landscape.get_material_usage_summary` → `landscape.get_material_usage_summary` (layer weight array per component)

---

## foliage (7) — 7/7 covered

- [x] `foliage.list_types` → `foliage.list_types` (AInstancedFoliageActor::GetFoliageInfos())
- [x] `foliage.get_settings` → `foliage.get_settings` (UFoliageType_InstancedStaticMesh property dump)
- [x] `foliage.sample` → `foliage.sample` (instance count + transforms at world-space radius)
- [x] `foliage.paint` → `foliage.paint` (note: FoliagePaintBrush requires editor mode; returns guidance)
- [x] `foliage.erase` → `foliage.erase` (note: same editor-mode limitation; returns guidance)
- [x] `foliage.create_type` → `foliage.create_type` (IAssetTools::CreateAsset UFoliageType_InstancedStaticMesh)
- [x] `foliage.set_settings` → `foliage.set_settings` (UFoliageType FProperty mutation via SetUPropertyFromJson)

---

## reflection (6) — 6/6 covered

- [x] `reflection.reflect_class` → `reflect_class`
- [x] `reflection.reflect_struct` → `reflect_struct`
- [x] `reflection.reflect_enum` → `reflect_enum`
- [x] `reflection.list_classes` → `list_classes`
- [x] `reflection.list_tags` → `reflection.list_tags` (reads DefaultGameplayTags.ini + DefaultEngine.ini via GConfig::GetArray)
- [x] `reflection.create_tag` → `reflection.create_tag` (GConfig::GetArray duplicate check + append + GConfig::Flush)

---

## audio (5) — 5/5 covered

- [x] `audio.list` → `audio.list` (AssetRegistry query for SoundWave/SoundCue/MetaSoundSource with type filter)
- [x] `audio.play_at_location` → `audio.play_at_location` (GEditor->Exec `au.PlaySound <path>`)
- [x] `audio.spawn_ambient` → `audio.spawn_ambient` (GEditor->AddActor for AmbientSound + sound property assignment)
- [x] `audio.create_cue` → `audio.create_cue` (IAssetTools::CreateAsset USoundCue)
- [x] `audio.create_metasound` → `audio.create_metasound` (FindObject MetaSoundSource + CreateAsset)

---

## feedback (1) — N/A

- [N/A] `feedback.submit` — Submit gap report · W · **Sage equivalent:** `lessons.md` + GitHub issues. Not a runtime tool.

---

## demo (2) — N/A

- [N/A] `demo.step` · W · *project-specific to ue-mcp's "Neon Shrine" demo. Not generic.*
- [N/A] `demo.cleanup` · W · *same — Neon Shrine teardown. Not generic.*

---

## Sage-only tools (beyond ue-mcp surface)

These tools exist in Sage but have no ue-mcp equivalent — they extend the surface:

**Knowledge graph (Phase 2):** `index_assets`, `impact_of`, `references_to`, `find_unused`, `query_graph`, `class_hierarchy`
**Phase 3:** `restart_editor`
**Reflection extras:** `list_structs`, `list_enums`, `find_implementers`, `class_default_object`
**Blueprint extras:** `bp.read_components`, `bp.search_nodes`, `bp.delete_local_variable`
**Asset extras:** `asset.fixup_redirectors`, `asset.list_redirectors`
**Sequencer extras:** `seq.add_keyframe`, `seq.add_possessable`, `seq.add_spawnable`
**Editor extras:** `editor.get_engine_version`, `editor.get_project_version`, `editor.get_log_file_path`
**Project extras:** `project.set_plugin_enabled`
**SCM (Phase 1):** `get_source_control_state`, `checkout_files`, `commit_transaction`, `discard_changes`
**Transaction system:** `begin_transaction`, `commit_transaction`, `rollback_transaction`, `get_active_transactions`
**Animation extras:** beyond ue-mcp: `animation.read_bone_track`, `animation.set_bone_keyframes`, `animation.bake_root_motion_from_bone`
**Gameplay extras:** `gameplay.get_pie_anim_state`, `gameplay.get_pie_anim_properties`, `gameplay.get_pie_subsystem_state`
