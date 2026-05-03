#include "phase4_schemas.h"
#include "mcp/tool_registry.h"
#include "mcp/tool.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

namespace {

using Tool = sage::mcp::Tool;

// Convenience: register one remote tool, warn on collision.
static void reg(sage::mcp::ToolRegistry& R, Tool t) {
    const auto name = t.name;
    if (auto r = R.registerTool(std::move(t)); !r.has_value()) {
        spdlog::warn("phase4: failed to register '{}'", name);
    }
}

// Build a minimal "object" schema: properties + optional required list.
//
// Why initializer_list<const char*> instead of nlohmann::json: brace-init like
// `{{"path"}}` got parsed as nested array `[["path"]]` and `{{"path","name"}}`
// as object `{"path":"name"}` (key:value pair) — both invalid JSON Schema.
// initializer_list disambiguates and forces a flat string array.
//
// Empty `obj({})` argument turns into nlohmann::json's null (not empty object),
// which fails JSON Schema validation; normalize via is_null() check.
static nlohmann::json obj(nlohmann::json props,
                          std::initializer_list<const char*> required = {}) {
    nlohmann::json s{{"type","object"},{"additionalProperties",false}};
    s["properties"] = props.is_null() ? nlohmann::json::object() : std::move(props);
    if (required.size() > 0) {
        auto req = nlohmann::json::array();
        for (const char* k : required) req.push_back(k);
        s["required"] = std::move(req);
    }
    return s;
}

static nlohmann::json str()  { return {{"type","string"}}; }
static nlohmann::json num()  { return {{"type","number"}}; }
static nlohmann::json i32()  { return {{"type","integer"}}; }
static nlohmann::json bln()  { return {{"type","boolean"}}; }
static nlohmann::json arr()  { return {{"type","array"}}; }
static nlohmann::json vec3() { return {{"type","array"},{"items",{{"type","number"}}},{"minItems",3},{"maxItems",3}}; }

}  // anonymous namespace

namespace sage::tools {

void registerPhase4Schemas(mcp::ToolRegistry& registry) {

    // ========================================================================
    // animation.* (46 tools)
    // ========================================================================

    reg(registry, Tool{.name="animation.list",
        .description="List animation assets (AnimSequence/Montage/BlendSpace/ABP). Filter by type and path.",
        .inputSchema=obj({{"path",str()},{"type",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.get_skeleton_info",
        .description="Return bone count, hierarchy, and reference pose for a USkeleton asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.list_skeletal_meshes",
        .description="List skeletal meshes optionally filtered by skeleton asset.",
        .inputSchema=obj({{"skeleton",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.list_sockets",
        .description="List sockets on a skeletal mesh.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.get_physics_asset",
        .description="Describe the physics asset (bodies and constraints) attached to a skeletal mesh.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_anim_blueprint",
        .description="Read an AnimBlueprint: parent skeleton, variable list, and anim-graph node summary.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_anim_graph",
        .description="Enumerate all nodes in an AnimBlueprint's anim graph (EventGraph + AnimGraph).",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_anim_blueprint_skeleton",
        .description="Reassign a different target skeleton to an AnimBlueprint and recompile.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_sequence",
        .description="Read an AnimSequence: length, rate, bone count, key count.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_bone_track",
        .description="Read per-bone key data from an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"bone",str()}},{"path","bone"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_sequence_properties",
        .description="Set rate scale, loop flag, interpolation mode, retarget source on an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"rate_scale",num()},{"loop",bln()},{"interpolation",str()},{"retarget_source",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_bone_keyframes",
        .description="Write bone transform keyframes into an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"bone",str()},{"keyframes",arr()}},{"path","bone","keyframes"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_root_motion",
        .description="Enable/disable root motion locking on an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"enabled",bln()}},{"path","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.bake_root_motion_from_bone",
        .description="Bake root motion from a reference bone into an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"bone",str()}},{"path","bone"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_sequence",
        .description="Create a new AnimSequence asset bound to a skeleton.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_montage",
        .description="Read an AnimMontage: sections, slot tracks, notifies.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_montage",
        .description="Create an AnimMontage asset from a skeleton.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_sequence",
        .description="Set the primary sequence referenced by an AnimMontage slot track.",
        .inputSchema=obj({{"path",str()},{"sequence",str()}},{"path","sequence"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_properties",
        .description="Set blend-in/out, rate scale, loop on an AnimMontage.",
        .inputSchema=obj({{"path",str()},{"blend_in",num()},{"blend_out",num()},{"rate_scale",num()},{"loop",bln()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_slot",
        .description="Set the slot track name on an AnimMontage.",
        .inputSchema=obj({{"path",str()},{"slot",str()}},{"path","slot"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_montage_section",
        .description="Add a named section to an AnimMontage at the given start time.",
        .inputSchema=obj({{"path",str()},{"section_name",str()},{"start_time",num()}},{"path","section_name","start_time"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_notify",
        .description="Add an AnimNotify event to a sequence or montage.",
        .inputSchema=obj({{"path",str()},{"notify_class",str()},{"time",num()}},{"path","notify_class","time"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_curve",
        .description="Add a named float curve to an AnimSequence. Note: SmartName API removed in UE 5.5+; returns guidance.",
        .inputSchema=obj({{"path",str()},{"curve_name",str()}},{"path","curve_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_blendspace",
        .description="Read a BlendSpace: axes, samples, and blend parameters.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_blendspace",
        .description="Create a BlendSpace1D or 2D asset.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()},{"dimensions",i32()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_composite",
        .description="Create an AnimComposite asset.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_state_machine",
        .description="Create a named state machine inside an AnimBlueprint's anim graph.",
        .inputSchema=obj({{"path",str()},{"name",str()}},{"path","name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_state_machine",
        .description="Read a state machine's states and transitions from an AnimBlueprint.",
        .inputSchema=obj({{"path",str()},{"state_machine",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_state",
        .description="Add a new state node to a state machine in an AnimBlueprint.",
        .inputSchema=obj({{"path",str()},{"state_machine",str()},{"state_name",str()}},{"path","state_machine","state_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_transition",
        .description="Add a transition between two states in an AnimBlueprint state machine.",
        .inputSchema=obj({{"path",str()},{"state_machine",str()},{"from",str()},{"to",str()}},{"path","state_machine","from","to"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_state_animation",
        .description="Link an animation sequence to a state node in an AnimBlueprint state machine.",
        .inputSchema=obj({{"path",str()},{"state_machine",str()},{"state",str()},{"animation",str()}},{"path","state_machine","state","animation"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_transition_blend",
        .description="Set blend duration and logic type on a state machine transition.",
        .inputSchema=obj({{"path",str()},{"state_machine",str()},{"from",str()},{"to",str()},{"duration",num()},{"logic",str()}},{"path","state_machine","from","to"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_ik_rig",
        .description="Create an IKRig definition asset bound to a skeleton.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_ik_rig",
        .description="Read an IKRig: goals, solvers, chains, and retarget chains.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_ik_retargeter",
        .description="Create an IKRetargeter asset linking source and target IKRigs.",
        .inputSchema=obj({{"path",str()},{"source_ik_rig",str()},{"target_ik_rig",str()}},{"path","source_ik_rig","target_ik_rig"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.list_control_rig_variables",
        .description="List exposed variables on a ControlRig asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_virtual_bone",
        .description="Add a virtual bone to a skeleton. Note: removed in UE 5.7; returns guidance.",
        .inputSchema=obj({{"skeleton",str()},{"source_bone",str()},{"target_bone",str()},{"name",str()}},{"skeleton","source_bone","target_bone"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.remove_virtual_bone",
        .description="Remove a virtual bone from a skeleton. Note: removed in UE 5.7; returns guidance.",
        .inputSchema=obj({{"skeleton",str()},{"bone_name",str()}},{"skeleton","bone_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.list_modifiers",
        .description="List available AnimationModifier subclasses.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.get_bone_transforms",
        .description="Get reference-pose transforms for listed bones on a skeleton.",
        .inputSchema=obj({{"skeleton",str()},{"bones",arr()}},{"skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_pose_search_database",
        .description="Create a PoseSearch database asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_pose_search_schema",
        .description="Assign a PoseSearchSchema asset to a PoseSearch database.",
        .inputSchema=obj({{"path",str()},{"schema",str()}},{"path","schema"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_pose_search_sequence",
        .description="Add an AnimSequence to a PoseSearch database.",
        .inputSchema=obj({{"path",str()},{"sequence",str()}},{"path","sequence"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.build_pose_search_index",
        .description="Build/rebuild the index of a PoseSearch database.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_pose_search_database",
        .description="Read a PoseSearch database: schema, sequences, and index status.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_anim_blueprint",
        .description="Create an AnimBlueprint asset bound to a skeleton.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // gameplay.* (45 tools)
    // ========================================================================

    reg(registry, Tool{.name="gameplay.set_collision_profile",
        .description="Set a collision preset (profile name) on a PrimitiveComponent.",
        .inputSchema=obj({{"actor",str()},{"component",str()},{"profile",str()}},{"actor","profile"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_simulate_physics",
        .description="Toggle physics simulation on a PrimitiveComponent.",
        .inputSchema=obj({{"actor",str()},{"component",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_collision_enabled",
        .description="Set collision mode (NoCollision/QueryOnly/PhysicsOnly/QueryAndPhysics) on a component.",
        .inputSchema=obj({{"actor",str()},{"component",str()},{"mode",str()}},{"actor","mode"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_physics_properties",
        .description="Set mass, linear damping, angular damping, and gravity scale on a physics body.",
        .inputSchema=obj({{"actor",str()},{"component",str()},{"mass",num()},{"linear_damping",num()},{"angular_damping",num()},{"gravity_scale",num()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.rebuild_navigation",
        .description="Rebuild the NavMesh for the current level.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_navmesh_info",
        .description="Return the navigation system settings and RecastNavMesh actor summary.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_navmesh_details",
        .description="Return full RecastNavMesh parameter dump.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.project_to_nav",
        .description="Project a 3D world point onto the NavMesh.",
        .inputSchema=obj({{"location",vec3()},{"agent_radius",num()}},{"location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.spawn_nav_modifier",
        .description="Place a NavModifierVolume actor in the level.",
        .inputSchema=obj({{"location",vec3()},{"extent",vec3()},{"area_class",str()}},{"location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_input_action",
        .description="Create a new UInputAction asset.",
        .inputSchema=obj({{"path",str()},{"value_type",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_input_mapping",
        .description="Create a new UInputMappingContext asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.list_input_assets",
        .description="List InputAction and InputMappingContext assets in the project.",
        .inputSchema=obj({{"path",str()},{"type",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.read_imc",
        .description="Read all key mappings from an InputMappingContext asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.list_input_mappings",
        .description="List all key→action bindings across all IMC assets.",
        .inputSchema=obj({{"path",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.add_imc_mapping",
        .description="Add a key mapping to an InputMappingContext.",
        .inputSchema=obj({{"path",str()},{"action",str()},{"key",str()}},{"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_mapping_modifiers",
        .description="Set modifier and trigger chains on an IMC key mapping.",
        .inputSchema=obj({{"path",str()},{"action",str()},{"key",str()},{"modifiers",arr()},{"triggers",arr()}},{"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.remove_imc_mapping",
        .description="Remove a key mapping from an InputMappingContext by action+key.",
        .inputSchema=obj({{"path",str()},{"action",str()},{"key",str()}},{"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_imc_mapping_key",
        .description="Rebind an existing IMC mapping to a new key.",
        .inputSchema=obj({{"path",str()},{"action",str()},{"old_key",str()},{"new_key",str()}},{"path","action","old_key","new_key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_imc_mapping_action",
        .description="Retarget an IMC mapping to a different InputAction asset.",
        .inputSchema=obj({{"path",str()},{"old_action",str()},{"new_action",str()},{"key",str()}},{"path","old_action","new_action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.list_behavior_trees",
        .description="List BehaviorTree assets in the project.",
        .inputSchema=obj({{"path",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_behavior_tree_info",
        .description="Return the root task, blackboard asset, and node count for a BehaviorTree.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.read_behavior_tree_graph",
        .description="Walk a BehaviorTree and return its composite/task node tree.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_blackboard",
        .description="Create a new BlackboardData asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_behavior_tree",
        .description="Create a new BehaviorTree asset.",
        .inputSchema=obj({{"path",str()},{"blackboard",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_eqs_query",
        .description="Create a new EnvQuery (EQS) asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.list_eqs_queries",
        .description="List all EnvQuery assets in the project.",
        .inputSchema=obj({{"path",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.add_perception",
        .description="Add an AIPerceptionComponent to a Blueprint actor.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.configure_sense",
        .description="Configure a sense (sight/hearing/damage) on an AIPerceptionComponent.",
        .inputSchema=obj({{"path",str()},{"sense",str()},{"config",{{"type","object"}}}},{"path","sense","config"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_state_tree",
        .description="Create a new StateTree asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.list_state_trees",
        .description="List StateTree assets in the project.",
        .inputSchema=obj({{"path",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.add_state_tree_component",
        .description="Add a StateTreeComponent to a Blueprint actor and bind a StateTree asset.",
        .inputSchema=obj({{"path",str()},{"state_tree",str()}},{"path","state_tree"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_smart_object_def",
        .description="Create a new SmartObjectDefinition asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.add_smart_object_component",
        .description="Add a SmartObjectComponent to a Blueprint actor.",
        .inputSchema=obj({{"path",str()},{"definition",str()}},{"path","definition"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.inspect_pie",
        .description="Inspect a PIE actor's component tree and property snapshot.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_pie_anim_state",
        .description="Get the UAnimInstance property snapshot for a PIE skeletal mesh actor.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_pie_anim_properties",
        .description="Get all UPROPERTY values on a PIE actor's UAnimInstance.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_pie_subsystem_state",
        .description="Get UPROPERTY values from a UGameInstanceSubsystem or UWorldSubsystem in PIE.",
        .inputSchema=obj({{"class",str()}},{"class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_game_mode",
        .description="Create a GameMode Blueprint subclassing AGameModeBase.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_game_state",
        .description="Create a GameState Blueprint subclassing AGameStateBase.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_player_controller",
        .description="Create a PlayerController Blueprint subclassing APlayerController.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_player_state",
        .description="Create a PlayerState Blueprint subclassing APlayerState.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.create_hud",
        .description="Create a HUD Blueprint subclassing AHUD.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_world_game_mode",
        .description="Override the default GameMode class in World Settings.",
        .inputSchema=obj({{"game_mode",str()}},{"game_mode"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.get_framework_info",
        .description="Return the GM/GS/PC/PS/HUD class names set in World Settings.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.apply_damage_in_pie",
        .description="Apply damage to an actor in the active PIE session.",
        .inputSchema=obj({{"actor",str()},{"damage",num()},{"damage_type",str()}},{"actor","damage"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // niagara.* (26 tools)
    // ========================================================================

    reg(registry, Tool{.name="niagara.list",
        .description="List UNiagaraSystem assets in the project.",
        .inputSchema=obj({{"path",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.get_info",
        .description="Return emitter list and parameter summary for a Niagara system.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.spawn",
        .description="Spawn a Niagara system at a world location in PIE.",
        .inputSchema=obj({{"path",str()},{"location",vec3()},{"auto_destroy",bln()}},{"path","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.set_parameter",
        .description="Set a user-exposed parameter (float/int/bool/color/vector) on a spawned Niagara component.",
        .inputSchema=obj({{"component",str()},{"name",str()},{"value",{{"description","scalar or array"}}}},{"component","name","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.create",
        .description="Create a new UNiagaraSystem asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.create_emitter",
        .description="Create a new UNiagaraEmitter asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.add_emitter",
        .description="Add an emitter handle to a Niagara system.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"name",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_emitters",
        .description="List emitter handles in a Niagara system.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.set_emitter_property",
        .description="Set a reflected property on a Niagara emitter handle.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"property",str()},{"value",{{"description","JSON value"}}}},{"path","emitter","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_modules",
        .description="List script modules of a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.get_emitter_info",
        .description="Return full property dump of a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_renderers",
        .description="List renderer properties on a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.add_renderer",
        .description="Add a renderer (Sprite/Mesh/Ribbon/Beam) to a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"renderer_class",str()}},{"path","emitter","renderer_class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.remove_renderer",
        .description="Remove a renderer by index from a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"index",i32()}},{"path","emitter","index"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.set_renderer_property",
        .description="Set a reflected property on a Niagara renderer.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"index",i32()},{"property",str()},{"value",{{"description","JSON value"}}}},{"path","emitter","index","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.inspect_data_interfaces",
        .description="Dump all data interfaces attached to a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.create_system_from_spec",
        .description="Create a Niagara system from a declarative JSON emitter spec.",
        .inputSchema=obj({{"path",str()},{"spec",{{"type","object"}}}},{"path","spec"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.get_compiled_hlsl",
        .description="Retrieve compiled HLSL for a Niagara script. Note: requires NiagaraEditor private API.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_system_parameters",
        .description="List user-exposed parameters on a Niagara system.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_module_inputs",
        .description="List variable inputs for a module script in a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.set_module_input",
        .description="Override a variable input on a Niagara script module.",
        .inputSchema=obj({{"path",str()},{"emitter",str()},{"variable",str()},{"value",{{"description","JSON value"}}}},{"path","emitter","variable","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.list_static_switches",
        .description="List static switch variables on a Niagara script.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.set_static_switch",
        .description="Override a static switch value on a Niagara script.",
        .inputSchema=obj({{"path",str()},{"switch",str()},{"value",{{"description","bool or int"}}}},{"path","switch","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.create_module_from_hlsl",
        .description="Create a Niagara script module from HLSL source. Note: requires compiler private API.",
        .inputSchema=obj({{"path",str()},{"hlsl",str()}},{"path","hlsl"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.create_scratch_module",
        .description="Create a scratch-pad script module for a Niagara emitter.",
        .inputSchema=obj({{"path",str()},{"emitter",str()}},{"path","emitter"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="niagara.batch",
        .description="Execute multiple niagara tool calls sequentially in one request.",
        .inputSchema=obj({{"calls",arr()}},{"calls"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // pcg.* (16 tools)
    // ========================================================================

    reg(registry, Tool{.name="pcg.list_graphs",
        .description="List PCGGraph assets in the project.",
        .inputSchema=obj({{"path",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.read_graph",
        .description="Read a PCGGraph: nodes and edges dump.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.read_node_settings",
        .description="Dump reflected settings properties of a PCG node.",
        .inputSchema=obj({{"path",str()},{"node_index",i32()}},{"path","node_index"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.get_components",
        .description="List all UPCGComponent instances in the editor world.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.get_component_details",
        .description="Return graph asset, owning actor, and bounds for a PCGComponent.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.create_graph",
        .description="Create a new PCGGraph asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.add_node",
        .description="Add a PCG node of the given settings class to a graph.",
        .inputSchema=obj({{"path",str()},{"settings_class",str()}},{"path","settings_class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.connect_nodes",
        .description="Add an edge between two PCG nodes by index and pin name.",
        .inputSchema=obj({{"path",str()},{"from_index",i32()},{"from_pin",str()},{"to_index",i32()},{"to_pin",str()}},{"path","from_index","to_index"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.set_node_settings",
        .description="Set a reflected property on a PCG node's settings object.",
        .inputSchema=obj({{"path",str()},{"node_index",i32()},{"property",str()},{"value",{{"description","JSON value"}}}},{"path","node_index","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.set_static_mesh_spawner_meshes",
        .description="Set the mesh entries on a PCGStaticMeshSpawnerSettings node.",
        .inputSchema=obj({{"path",str()},{"node_index",i32()},{"meshes",arr()}},{"path","node_index","meshes"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.remove_node",
        .description="Remove a PCG node by index from a graph.",
        .inputSchema=obj({{"path",str()},{"node_index",i32()}},{"path","node_index"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.execute",
        .description="Trigger UPCGComponent::Generate on an actor's PCG component.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.force_regenerate",
        .description="Cleanup and regenerate a PCG component.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.cleanup",
        .description="Cleanup (clear output) a PCG component without regenerating.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.toggle_graph",
        .description="Enable or disable a PCG component's graph execution.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="pcg.add_volume",
        .description="Place an APCGVolume actor with a UPCGComponent in the level.",
        .inputSchema=obj({{"location",vec3()},{"extent",vec3()},{"graph",str()}},{"location"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // landscape.* (11 tools)
    // ========================================================================

    reg(registry, Tool{.name="landscape.get_info",
        .description="Return bounds, scale, component count, and material for the level landscape.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.list_layers",
        .description="List layer allocations on all landscape components.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.sample",
        .description="Sample the landscape heightmap at a world-space XY position.",
        .inputSchema=obj({{"x",num()},{"y",num()}},{"x","y"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.list_splines",
        .description="List landscape spline control points and segments.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.get_component",
        .description="Return info for the landscape component at section coordinates (X, Y).",
        .inputSchema=obj({{"section_x",i32()},{"section_y",i32()}},{"section_x","section_y"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.sculpt",
        .description="Sculpt landscape height at a location. Note: requires LandscapeEditor mode; returns guidance.",
        .inputSchema=obj({{"location",vec3()},{"radius",num()},{"strength",num()},{"mode",str()}},{"location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.paint_layer",
        .description="Paint a layer weight at a location. Note: requires LandscapeEditor mode; returns guidance.",
        .inputSchema=obj({{"location",vec3()},{"radius",num()},{"layer",str()},{"weight",num()}},{"location","layer"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.set_material",
        .description="Set the landscape material asset.",
        .inputSchema=obj({{"material",str()}},{"material"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.add_layer_info",
        .description="Create a LandscapeLayerInfoObject asset for a named layer.",
        .inputSchema=obj({{"path",str()},{"layer_name",str()}},{"path","layer_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.import_heightmap",
        .description="Import a PNG or RAW heightmap file into the landscape.",
        .inputSchema=obj({{"file",str()},{"heightmap_scale",num()}},{"file"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="landscape.get_material_usage_summary",
        .description="Return layer weight statistics per landscape component.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // foliage.* (7 tools)
    // ========================================================================

    reg(registry, Tool{.name="foliage.list_types",
        .description="List all foliage types registered in the level.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.get_settings",
        .description="Return property dump for a UFoliageType_InstancedStaticMesh asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.sample",
        .description="Return instance count and transforms within a world-space radius.",
        .inputSchema=obj({{"path",str()},{"center",vec3()},{"radius",num()}},{"center","radius"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.paint",
        .description="Paint foliage at a location. Note: requires FoliagePaintBrush editor mode; returns guidance.",
        .inputSchema=obj({{"path",str()},{"location",vec3()},{"radius",num()},{"density",num()}},{"path","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.erase",
        .description="Erase foliage at a location. Note: requires FoliagePaintBrush editor mode; returns guidance.",
        .inputSchema=obj({{"path",str()},{"location",vec3()},{"radius",num()}},{"path","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.create_type",
        .description="Create a UFoliageType_InstancedStaticMesh asset.",
        .inputSchema=obj({{"path",str()},{"mesh",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="foliage.set_settings",
        .description="Set reflected properties on a UFoliageType asset.",
        .inputSchema=obj({{"path",str()},{"properties",{{"type","object"}}}},{"path","properties"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // gas.* (9 tools)
    // ========================================================================

    reg(registry, Tool{.name="gas.add_asc",
        .description="Add an AbilitySystemComponent to a Blueprint actor.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.create_attribute_set",
        .description="Create an AttributeSet subclass. Note: requires C++ subclass; returns guidance.",
        .inputSchema=obj({{"path",str()},{"attributes",arr()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.add_attribute",
        .description="Add an attribute to an AttributeSet. Note: requires C++ subclass; returns guidance.",
        .inputSchema=obj({{"attribute_set",str()},{"name",str()},{"default_value",num()}},{"attribute_set","name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.create_ability",
        .description="Create a GameplayAbility Blueprint asset.",
        .inputSchema=obj({{"path",str()},{"parent",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.set_ability_tags",
        .description="Set ability, cancel, and block tags on a GameplayAbility Blueprint.",
        .inputSchema=obj({{"path",str()},{"ability_tags",arr()},{"cancel_tags",arr()},{"block_tags",arr()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.create_effect",
        .description="Create a GameplayEffect asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.set_effect_modifier",
        .description="Set a modifier property on a GameplayEffect via FProperty reflection.",
        .inputSchema=obj({{"path",str()},{"property",str()},{"value",{{"description","JSON value"}}}},{"path","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.create_cue",
        .description="Create a GameplayCueNotify_Static asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gas.get_info",
        .description="Return AbilitySystemComponent info: active abilities and effects for an actor.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // networking.* (11 tools)
    // ========================================================================

    reg(registry, Tool{.name="networking.set_replicates",
        .description="Enable or disable actor replication.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_property_replicated",
        .description="Mark a property as replicated. Note: requires UPROPERTY(Replicated) in source code.",
        .inputSchema=obj({{"path",str()},{"property",str()}},{"path","property"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.configure_net_frequency",
        .description="Set NetUpdateFrequency on an actor.",
        .inputSchema=obj({{"actor",str()},{"frequency",num()}},{"actor","frequency"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_dormancy",
        .description="Set network dormancy mode (DORM_Never/Awake/DormantAll/DormantPartial/Initial).",
        .inputSchema=obj({{"actor",str()},{"dormancy",str()}},{"actor","dormancy"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_net_load_on_client",
        .description="Set bNetLoadOnClient on an actor.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_always_relevant",
        .description="Set bAlwaysRelevant on an actor.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_only_relevant_to_owner",
        .description="Set bOnlyRelevantToOwner on an actor.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.configure_cull_distance",
        .description="Set the net cull distance (meters) on an actor.",
        .inputSchema=obj({{"actor",str()},{"distance",num()}},{"actor","distance"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_priority",
        .description="Set NetPriority on an actor.",
        .inputSchema=obj({{"actor",str()},{"priority",num()}},{"actor","priority"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.set_replicate_movement",
        .description="Enable or disable movement replication on an actor.",
        .inputSchema=obj({{"actor",str()},{"enabled",bln()}},{"actor","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="networking.get_info",
        .description="Return all replication properties for an actor in one call.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // audio.* (5 tools)
    // ========================================================================

    reg(registry, Tool{.name="audio.list",
        .description="List SoundWave/SoundCue/MetaSoundSource assets with optional type filter.",
        .inputSchema=obj({{"path",str()},{"type",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.play_at_location",
        .description="Play a sound asset at a world location via au.PlaySound console command.",
        .inputSchema=obj({{"path",str()},{"location",vec3()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.spawn_ambient",
        .description="Spawn an AmbientSound actor with the given sound asset at a location.",
        .inputSchema=obj({{"sound",str()},{"location",vec3()},{"label",str()}},{"sound","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.create_cue",
        .description="Create a USoundCue asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.create_metasound",
        .description="Create a MetaSoundSource asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // level.* extensions (22 missing tools)
    // ========================================================================

    reg(registry, Tool{.name="level.get_outliner",
        .description="List actors in the current level with class, label, location, and component count.",
        .inputSchema=obj({{"class_filter",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.get_actor_details",
        .description="Full UPROPERTY dump for an actor in the level.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.load",
        .description="Load a level asset (replaces persistent level). Rejects during PIE.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.list",
        .description="List UWorld assets in a directory.",
        .inputSchema=obj({{"path",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.create",
        .description="Create a new level asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.spawn_volume",
        .description="Spawn a volume actor (BlockingVolume/TriggerVolume/PhysicsVolume/etc) at a location.",
        .inputSchema=obj({{"class",str()},{"location",vec3()},{"extent",vec3()},{"label",str()}},{"class","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.list_volumes",
        .description="List volume actors in the level, optionally filtered by class name.",
        .inputSchema=obj({{"class_filter",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_volume_properties",
        .description="Set reflected properties on a volume actor.",
        .inputSchema=obj({{"actor",str()},{"properties",{{"type","object"}}}},{"actor","properties"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.spawn_light",
        .description="Spawn a light actor (DirectionalLight/PointLight/SpotLight/RectLight).",
        .inputSchema=obj({{"type",str()},{"location",vec3()},{"rotation",vec3()},{"label",str()}},{"type","location"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_light_properties",
        .description="Set intensity, color, and rotation on a light actor's ULightComponent.",
        .inputSchema=obj({{"actor",str()},{"intensity",num()},{"color",arr()},{"rotation",vec3()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_fog_properties",
        .description="Set ExponentialHeightFog properties (fog density, height, inscattering color).",
        .inputSchema=obj({{"density",num()},{"height_falloff",num()},{"inscattering_color",arr()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.get_actors_by_class",
        .description="Return all actors of a given class in the level.",
        .inputSchema=obj({{"class",str()},{"max_results",i32()}},{"class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.count_actors_by_class",
        .description="Return a histogram of actor class name → count for the level.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.get_runtime_virtual_texture_summary",
        .description="List RuntimeVirtualTextureVolume actors and their material usage.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_water_body_property",
        .description="Set a reflected property on a WaterBodyComponent.",
        .inputSchema=obj({{"actor",str()},{"property",str()},{"value",{{"description","JSON value"}}}},{"actor","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.build_lighting",
        .description="Build level lighting with optional quality (Preview/Medium/High/Production).",
        .inputSchema=obj({{"quality",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.get_spline_info",
        .description="Return the number of spline points and their locations for a spline actor.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_spline_points",
        .description="Replace the spline control points on a spline actor.",
        .inputSchema=obj({{"actor",str()},{"points",arr()}},{"actor","points"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_actor_material",
        .description="Set a material on the first primitive component of an actor.",
        .inputSchema=obj({{"actor",str()},{"material",str()},{"slot",i32()}},{"actor","material"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.set_world_settings",
        .description="Set AWorldSettings reflected properties (gravity, timer, KillZ, etc).",
        .inputSchema=obj({{"properties",{{"type","object"}}}},{"properties"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.get_actor_bounds",
        .description="Return origin and extent (AABB) for an actor.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.resolve_actor",
        .description="Resolve an actor's internal name to its editor label.",
        .inputSchema=obj({{"name",str()}},{"name"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // mat.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="mat.disconnect",
        .description="Disconnect an expression's output from a material property or other expression input.",
        .inputSchema=obj({{"path",str()},{"expression",str()},{"property",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.create",
        .description="Create a new base UMaterial asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.set_blend_mode",
        .description="Set material blend mode (Opaque/Masked/Translucent/Additive/Modulate).",
        .inputSchema=obj({{"path",str()},{"mode",str()}},{"path","mode"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.list_expression_types",
        .description="List all available UMaterialExpression subclasses.",
        .inputSchema=obj({{"filter",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.recompile",
        .description="Force recompile the shader for a material asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.duplicate",
        .description="Duplicate a material asset to a new path.",
        .inputSchema=obj({{"path",str()},{"dest",str()}},{"path","dest"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.get_shader_stats",
        .description="Return shader compilation stats for a material. Note: instruction count API removed in UE 5.7.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.export_graph",
        .description="Export a material's expression graph as a JSON node+edge list.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.import_graph",
        .description="Rebuild a material from a previously exported JSON node+edge spec.",
        .inputSchema=obj({{"path",str()},{"graph",{{"type","object"}}}},{"path","graph"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.build_graph",
        .description="Create a material from a declarative JSON spec (expressions + connections + property bindings) in one call.",
        .inputSchema=obj({{"path",str()},{"spec",{{"type","object"}}}},{"path","spec"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.render_preview",
        .description="Render a material preview PNG via SceneCapture2D. Returns base64 PNG or note about headless limitation.",
        .inputSchema=obj({{"path",str()},{"resolution",i32()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.begin_transaction",
        .description="Open an FScopedTransaction for a sequence of material graph edits.",
        .inputSchema=obj({{"label",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.end_transaction",
        .description="Commit the open material editing transaction.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // editor.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="editor.hot_reload",
        .description="Trigger C++ hot reload. Note: platform availability varies; returns guidance on Mac.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.get_perf_stats",
        .description="Return frame time, FPS, and viewport resolution from the editor.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.set_scalability",
        .description="Set editor/game quality scalability level (0-3 or low/medium/high/epic/cinematic).",
        .inputSchema=obj({{"level",{{"description","0-3 integer or preset name string"}}}},{"level"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.capture_scene_png",
        .description="Capture a scene view to PNG via SceneCapture2D. Returns file path.",
        .inputSchema=obj({{"location",vec3()},{"rotation",vec3()},{"fov",num()},{"output_path",str()}},{"location","rotation"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.play_sequence",
        .description="Play, stop, or pause a Level Sequence in the editor.",
        .inputSchema=obj({{"path",str()},{"action",str()}},{"path","action"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.validate_assets",
        .description="Run UEditorValidatorSubsystem over a directory or asset list.",
        .inputSchema=obj({{"path",str()},{"assets",arr()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.cook_content",
        .description="Cook content for a platform via UAT. Returns note about async cook status.",
        .inputSchema=obj({{"platform",str()},{"maps",arr()}},{"platform"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.get_message_log",
        .description="Return recent entries from the FMessageLog system.",
        .inputSchema=obj({{"category",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="editor.open_asset",
        .description="Open an asset in its editor using UAssetEditorSubsystem.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // project.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="project.set_project",
        .description="Write GeneralProjectSettings (project name/company/description) to DefaultGame.ini.",
        .inputSchema=obj({{"name",str()},{"company",str()},{"description",str()},{"version",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.read_module",
        .description="Read a module's Build.cs content and return header/source file counts.",
        .inputSchema=obj({{"module",str()}},{"module"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.search_engine_cpp",
        .description="Search the engine source (Runtime/Editor/Developer) for a query string.",
        .inputSchema=obj({{"query",str()},{"max_results",i32()},{"category",str()}},{"query"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.generate_project_files",
        .description="Regenerate project files via UBT. Returns shell command guidance.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.create_cpp_class",
        .description="Create a UCLASS .h + .cpp pair in a project module. "
                     "Required: class_name. Optional: parent_class (default UObject), "
                     "module (default = project name), subfolder (under Source/<Module>/). "
                     "Auto UHT compliance: applies conventional prefix from parent "
                     "('A' for AActor descendants, 'U' for UObject, ...) and includes "
                     "the parent's engine header for ~30 common base classes (AActor, "
                     "APawn, ACharacter, AGameModeBase, UActorComponent, USceneComponent, "
                     "UCharacterMovementComponent, UAnimInstance, UUserWidget, etc.). "
                     "For custom/unknown bases pass parent_header (engine-relative path "
                     "like 'GameFramework/Character.h'). "
                     "If the module's Build.cs doesn't exist, pass bootstrap_module=true "
                     "to scaffold a fresh native module: writes Build.cs + module .h/.cpp + "
                     "<Project>.Target.cs + <Project>Editor.Target.cs and patches the "
                     ".uproject Modules[] array. Editor restart required afterward to "
                     "compile (Mac: no live coding, must rebuild from terminal or via "
                     "restart_editor MCP tool).",
        .inputSchema=obj({{"class_name",str()},{"parent_class",str()},
                          {"parent_header",str()},{"module",str()},
                          {"subfolder",str()},{"bootstrap_module",bln()}},
                         {"class_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.list_project_modules",
        .description="List native modules declared in the .uproject JSON Modules array.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.live_coding_compile",
        .description="Trigger a Live Coding compile via GEditor->Exec('LiveCoding.Compile').",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.write_cpp_file",
        .description="Write or overwrite a .h/.cpp/.inl file under the project Source/ directory.",
        .inputSchema=obj({{"path",str()},{"content",str()}},{"path","content"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="project.add_module_dependency",
        .description="Append a module name to PublicDependencyModuleNames "
                     "(default) or PrivateDependencyModuleNames (private=true) "
                     "in a module's Build.cs file. Idempotent — returns "
                     "{already_present:true} if the dependency string is "
                     "already in the file. Insertion is array-literal-aware: "
                     "places the new entry before the closing `}` of the "
                     "AddRange/Add invocation, with comma handling for both "
                     "trailing-comma and no-trailing-comma styles.",
        .inputSchema=obj({{"module",str()},{"dependency",str()},{"private",bln()}},
                         {"module","dependency"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // widget.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="widget.get_details",
        .description="Full property dump for a WidgetBlueprint via TFieldIterator.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.read_animations",
        .description="List WidgetAnimation assets declared in a WidgetBlueprint.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.create_utility_widget",
        .description="Create an EditorUtilityWidget Blueprint asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.run_utility_widget",
        .description="Run an EditorUtilityWidget via UEditorUtilitySubsystem. Returns note about API.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.create_utility_blueprint",
        .description="Create an EditorUtilityBlueprint (GlobalEditorUtilityBase) asset.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.run_utility_blueprint",
        .description="Find and invoke the Run/Execute function on an EditorUtilityBlueprint.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.move_widget",
        .description="Move a widget from one parent panel to another inside a WidgetBlueprint.",
        .inputSchema=obj({{"path",str()},{"widget",str()},{"new_parent",str()}},{"path","widget","new_parent"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.list_classes",
        .description="List all UWidget subclasses available in the engine/project.",
        .inputSchema=obj({{"filter",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.list_runtime",
        .description="List UUserWidget instances currently in the PIE viewport.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.get_runtime",
        .description="Get property values for a named UUserWidget in the PIE viewport.",
        .inputSchema=obj({{"name",str()}},{"name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.get_runtime_delegates",
        .description="List FMulticastDelegateProperty names on a runtime UUserWidget.",
        .inputSchema=obj({{"name",str()}},{"name"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // widget.anim.* — UMG Widget Animation authoring (Phase 4.11-r3)
    // CommonAIExport parity: 4 tools that author UWidgetBlueprint::Animations
    // (TArray<UWidgetAnimation*>). Each animation owns a UMovieScene and a
    // TArray<FWidgetAnimationBinding> mapping widget names → possessable GUIDs.
    // ========================================================================

    reg(registry, Tool{.name="widget.anim.create",
        .description="Create a new UWidgetAnimation on a WidgetBlueprint. "
                     "Allocates a UMovieScene with `frame_rate` (default 60 FPS) "
                     "display rate and `duration_seconds` (default 5s) playback "
                     "range. Returns {path, name, frame_rate, duration_seconds, "
                     "animation_count}. -32602 if the blueprint is missing or "
                     "the animation name already exists.",
        .inputSchema=obj({
            {"path",str()},
            {"name",str()},
            {"frame_rate",i32()},
            {"duration_seconds",num()},
        },{"path","name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.anim.bind",
        .description="Bind a widget from the WidgetTree to a UWidgetAnimation. "
                     "Adds a possessable to the MovieScene and an "
                     "FWidgetAnimationBinding entry. Idempotent — if the "
                     "widget is already bound, returns the existing GUID with "
                     "`already_bound: true`. Returns {path, anim_name, "
                     "widget_name, binding_guid, is_root, binding_count}.",
        .inputSchema=obj({
            {"path",str()},
            {"anim_name",str()},
            {"widget_name",str()},
        },{"path","anim_name","widget_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.anim.add_track",
        .description="Add a property track to a binding. Currently only "
                     "`track_type: 'float'` is supported (UMovieSceneFloatTrack); "
                     "more types follow once smoke-tested. Creates an empty "
                     "section spanning the playback range. Returns {path, "
                     "anim_name, binding_guid, property_name, track_type, "
                     "section_count}.",
        .inputSchema=obj({
            {"path",str()},
            {"anim_name",str()},
            {"binding_guid",str()},
            {"property_name",str()},
            {"track_type",str()},
        },{"path","anim_name","binding_guid","property_name"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // world.* — UWorld/Map structural exporter (Phase 4.6-r4)
    // ========================================================================

    reg(registry, Tool{.name="world.export",
        .description="Export the currently-loaded editor world as a "
                     "structured snapshot. Returns {world_path, world_name, "
                     "actors:[{name,class,path,folder?,transform:{location,"
                     "rotation,scale},tags?,components?,properties?,"
                     "property_count?}], returned, total, truncated, "
                     "is_world_partition, level_bounds:{min,max}, "
                     "streaming_levels:[{name,class,loaded,visible,...}], "
                     "streaming_count, world_settings?}. "
                     "Optional `path` is a safety check — if supplied and "
                     "doesn't match the current PersistentLevel, returns "
                     "-32602 telling the caller to load the map first "
                     "(production projects: never auto-switches maps). "
                     "Optional `actor_class_filter` (top-level asset path) "
                     "narrows enumeration. `include_components` lists "
                     "{name,class}; `include_actor_props` runs the full "
                     "instanced-recursion property reader (depth 2); "
                     "`include_world_settings` adds the AWorldSettings "
                     "reflected props (depth 3). `max_actors` defaults "
                     "to 10000 (range 1..200000).",
        .inputSchema=obj({
            {"path",str()},
            {"actor_class_filter",str()},
            {"include_components",bln()},
            {"include_actor_props",bln()},
            {"include_world_settings",bln()},
            {"max_actors",i32()},
        }),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // audio.read_* — Audio asset structural dumpers (Phase 4.x)
    // CommonAIExport parity: 7 read-only tools that resolve an audio asset,
    // verify the expected class (dynamically — AudioModulation plugin may not
    // be loaded), and emit reflected properties. SoundClass/SoundSubmix add
    // a flat `children` path array for hierarchy traversal. All seven accept
    // the same `recurse_instanced` + `max_depth` flags as asset.read_properties.
    // ========================================================================

    auto audioReadSchema = [&](){
        return obj({
            {"path",str()},
            {"recurse_instanced",bln()},
            {"max_depth",i32()},
        },{"path"});
    };

    reg(registry, Tool{.name="audio.read_sound_class",
        .description="Dump a USoundClass asset. Returns {path, class, "
                     "properties:{Volume,Pitch,...}, count, children:[paths]}. "
                     "`children` is the explicit ChildClasses array (parent "
                     "→ child hierarchy). -32602 if path doesn't resolve to a "
                     "USoundClass.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_sound_submix",
        .description="Dump a USoundSubmixBase asset (covers USoundSubmix, "
                     "USoundfieldSubmix, UEndpointSubmix). Returns the same "
                     "shape as read_sound_class with `children` reading "
                     "ChildSubmixes — the routing tree. -32602 if path doesn't "
                     "resolve.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_sound_concurrency",
        .description="Dump a USoundConcurrency asset (max active sounds, "
                     "voice stealing rules). Returns {path, class, properties, "
                     "count}.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_sound_attenuation",
        .description="Dump a USoundAttenuation asset (spatialisation, falloff "
                     "curves, occlusion, focus, reverb). Returns {path, class, "
                     "properties, count}. Curve points come through as "
                     "structured property values via the FRichCurve struct "
                     "shorthand.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_control_bus",
        .description="Dump a USoundControlBus asset (AudioModulation plugin). "
                     "Returns {path, class, properties, count}. -32602 if the "
                     "AudioModulation module/plugin isn't loaded.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_control_bus_mix",
        .description="Dump a USoundControlBusMix asset (AudioModulation "
                     "plugin) — the bus → mix value mapping. Returns {path, "
                     "class, properties, count}. -32602 if AudioModulation "
                     "isn't loaded.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="audio.read_modulation_patch",
        .description="Dump a USoundModulationPatch asset (AudioModulation "
                     "plugin) — modulator wiring + curves. Returns {path, "
                     "class, properties, count}. -32602 if AudioModulation "
                     "isn't loaded.",
        .inputSchema=audioReadSchema(),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.anim.add_keyframe",
        .description="Add a cubic-interpolated keyframe to a float track on "
                     "a binding. Time is in seconds (converted via the "
                     "MovieScene's tick resolution). If the keyframe is past "
                     "the current playback end, the playback range expands "
                     "to include it. Returns {path, anim_name, binding_guid, "
                     "property_name, time_seconds, value, key_count}.",
        .inputSchema=obj({
            {"path",str()},
            {"anim_name",str()},
            {"binding_guid",str()},
            {"property_name",str()},
            {"time_seconds",num()},
            {"value",num()},
        },{"path","anim_name","binding_guid","property_name","time_seconds","value"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // bp.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="bp.read_graph_summary",
        .description="Lightweight summary of a Blueprint function graph: node count and pin type list (≤10KB).",
        .inputSchema=obj({{"path",str()},{"function",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="bp.set_actor_tick_settings",
        .description="Set CDO tick interval, tick group, and start-with-tick-enabled on a Blueprint actor.",
        .inputSchema=obj({{"path",str()},{"enabled",bln()},{"interval",num()},{"group",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="bp.create_function",
        .description="Create a new user function in a Blueprint (alias for bp.add_function).",
        .inputSchema=obj({{"path",str()},{"name",str()},{"access",str()}},{"path","name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="bp.duplicate",
        .description="Duplicate a Blueprint asset to a new content path.",
        .inputSchema=obj({{"path",str()},{"dest",str()}},{"path","dest"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="bp.full_dump",
        .description="Atomic snapshot of a Blueprint's full state in one call: "
                     "header (parent, generated class, BP type), variables (name, "
                     "type, default, flags, category), components (SCS hierarchy "
                     "+ optional defaults), functions (params, locals, optional "
                     "graph node detail, optional T3D), event_dispatchers, "
                     "interfaces, cdo_properties, dependencies (assets + class "
                     "refs). Required: path. Optional: output_path (project-"
                     "relative or absolute; pretty-prints JSON to file and "
                     "returns the absolute path). Defaults: "
                     "include_function_graphs=true, include_referenced_assets=true, "
                     "include_component_defaults=true, include_t3d=false. "
                     "Designed as a destructive-change safety net — capture "
                     "before BP→C++ conversion / delete / restructure, diff or "
                     "audit afterward. Cevap schema_version='1' + captured_at "
                     "(ISO8601) içerir.",
        .inputSchema=obj({{"path",str()},
                          {"output_path",str()},
                          {"include_function_graphs",bln()},
                          {"include_t3d",bln()},
                          {"include_referenced_assets",bln()},
                          {"include_component_defaults",bln()}},
                         {"path"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // asset.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="asset.recenter_pivot",
        .description="Recenter a static mesh pivot. Note: vertex-level operation; returns guidance on exact bounds-center.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="asset.search_fts",
        .description="Full-text search over asset names. Exact FTS5 ranking available in sage-server KuzuDB.",
        .inputSchema=obj({{"query",str()},{"max_results",i32()},{"path",str()}},{"query"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="asset.reindex_fts",
        .description="Trigger a full FTS index rebuild in sage-server KuzuDB. Returns confirmation.",
        .inputSchema=obj({}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="asset.set_mesh_nav",
        .description="Set nav-relevant flags (bCanEverAffectNavigation) on a static or skeletal mesh.",
        .inputSchema=obj({{"path",str()},{"enabled",bln()}},{"path","enabled"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // seq.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="seq.add_keyframe",
        .description="Add a keyframe to a specific track/channel in a Level Sequence. Note: requires track-specific API.",
        .inputSchema=obj({{"path",str()},{"track",str()},{"time",num()},{"value",{{"description","JSON value"}}}},{"path","track","time","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="seq.add_possessable",
        .description="Add a possessable binding (existing level actor) to a Level Sequence.",
        .inputSchema=obj({{"path",str()},{"actor",str()}},{"path","actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="seq.add_spawnable",
        .description="Add a spawnable binding (template object) to a Level Sequence.",
        .inputSchema=obj({{"path",str()},{"class",str()},{"name",str()}},{"path","class"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // reflection.* extensions (missing tools)
    // ========================================================================

    reg(registry, Tool{.name="reflection.list_tags",
        .description="List all GameplayTags declared in DefaultGameplayTags.ini and DefaultEngine.ini.",
        .inputSchema=obj({{"filter",str()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="reflection.create_tag",
        .description="Create a new GameplayTag entry in DefaultGameplayTags.ini.",
        .inputSchema=obj({{"tag",str()},{"comment",str()}},{"tag"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // _scan_asset_registry (internal Phase 2 helper)
    // ========================================================================

    reg(registry, Tool{.name="_scan_asset_registry",
        .description="Internal: full asset registry scan used by the knowledge graph indexer.",
        .inputSchema=obj({{"path",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

}  // registerPhase4Schemas

}  // namespace sage::tools
