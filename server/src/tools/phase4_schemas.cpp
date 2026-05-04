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
        .description="[NOT IMPLEMENTED] Write bone transform keyframes into an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"bone",str()},{"keyframes",arr()}},{"path","bone","keyframes"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_root_motion",
        .description="Enable/disable root motion locking on an AnimSequence.",
        .inputSchema=obj({{"path",str()},{"enabled",bln()}},{"path","enabled"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.bake_root_motion_from_bone",
        .description="[NOT IMPLEMENTED] Bake root motion from a reference bone into an AnimSequence.",
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
        .description="Create an AnimMontage asset from a skeleton. Optional `sequence_path` (UAnimSequence) seeds the first slot track.",
        .inputSchema=obj({{"path",str()},{"skeleton",str()},{"sequence_path",str()}},{"path","skeleton"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_sequence",
        .description="[NOT IMPLEMENTED] Set the primary sequence referenced by an AnimMontage slot track.",
        .inputSchema=obj({{"path",str()},{"sequence",str()}},{"path","sequence"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_properties",
        .description="Set blend-in/out, rate scale, loop on an AnimMontage.",
        .inputSchema=obj({{"path",str()},{"blend_in",num()},{"blend_out",num()},{"rate_scale",num()},{"loop",bln()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_montage_slot",
        .description="Set the slot track name on an AnimMontage. Optional `slot_index` (default 0) targets a specific slot when the montage has multiple.",
        .inputSchema=obj({{"path",str()},{"slot",str()},{"slot_index",i32()}},{"path","slot"}),
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
        .description="Add a named float curve to an AnimSequence via IAnimationDataController (UE 5.5+ canonical path). Returns {added: bool}.",
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
        .description="Create a named state machine sub-graph inside an AnimBlueprint's AnimGraph (UAnimationStateMachineGraph + UAnimationStateMachineSchema). Spawns a UAnimGraphNode_StateMachine in the AnimGraph and optionally wires it to the AnimGraph Output Pose root. Returns {state_machine_name, state_machine_node_id, connected_to_root, compiled}. -32602 if name already exists.",
        .inputSchema=obj({{"path",str()},{"name",str()},{"connect_to_root",bln()},{"compile",bln()}},{"path","name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.read_state_machine",
        .description="Read a state machine's states and transitions from an AnimBlueprint.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_state",
        .description="Add a new UAnimStateNode to a state machine sub-graph. The state's BoundGraph (the inner anim graph driving the state's output pose) is renamed to match. Returns {state_id (FGuid digits), state_name, x, y}. Use the returned state_id in add_transition / set_state_animation calls.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"state_name",str()},{"x",num()},{"y",num()}},{"path","state_machine_name","state_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_transition",
        .description="Add a UAnimStateTransitionNode between two states (matched by FGuid id from add_state). Wires from_state.OutputPose -> transition.InputPose -> to_state.InputPose. Returns {transition_id, from_state_id, to_state_id}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"from_state_id",str()},{"to_state_id",str()}},{"path","state_machine_name","from_state_id","to_state_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_state_animation",
        .description="Drive a state's output pose with the given UAnimSequenceBase via a UAnimGraphNode_SequencePlayer in the state's BoundGraph (any pre-existing sequence players are removed for idempotency). Returns {state_id, animation, connected, player_node_id}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"state_id",str()},{"animation",str()},{"loop",bln()}},{"path","state_machine_name","state_id","animation"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_transition_blend",
        .description="Set the CrossfadeDuration on a UAnimStateTransitionNode (matched by FGuid id from add_transition). Returns {transition_id, blend_time}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"transition_id",str()},{"blend_time",num()}},{"path","state_machine_name","transition_id"}),
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
        .description="[NOT IMPLEMENTED] Assign a PoseSearchSchema asset to a PoseSearch database.",
        .inputSchema=obj({{"path",str()},{"schema",str()}},{"path","schema"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_pose_search_sequence",
        .description="[NOT IMPLEMENTED] Add an AnimSequence to a PoseSearch database.",
        .inputSchema=obj({{"path",str()},{"sequence",str()}},{"path","sequence"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.build_pose_search_index",
        .description="[NOT IMPLEMENTED] Build/rebuild the index of a PoseSearch database.",
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

    // ----- Phase 4-r6 (Lyra Sage Gap #17/#18) -------------------------------

    reg(registry, Tool{.name="animation.create_anim_notify",
        .description="Create a UAnimNotify subclass Blueprint asset. The BP starts blank; wire `Received_Notify` via Sage BP graph tools (bp.add_event_node + bp.add_function_call). Optional `parent_class` lets you derive from an existing UAnimNotify subclass (e.g. /Script/Engine.AnimNotify_PlayParticleEffect). Returns {path, parent_class}. -32602 if parent_class isn't a UAnimNotify subclass.",
        .inputSchema=obj({{"path",str()},{"parent_class",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.create_anim_notify_state",
        .description="Create a UAnimNotifyState subclass Blueprint asset. The BP starts blank; wire `Received_NotifyBegin` / `Received_NotifyTick` / `Received_NotifyEnd` via Sage BP graph tools. Optional `parent_class` defaults to /Script/Engine.AnimNotifyState. Returns {path, parent_class}.",
        .inputSchema=obj({{"path",str()},{"parent_class",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_blendspace_sample",
        .description="Append a single anim sample to a UBlendSpace at the given (x, y) coordinate. For 1D blendspaces only `x` is used. Accepts either {x, y} scalar pair or {position: [a, b]} array form. Returns {added: bool, sample_count, x, y}.",
        .inputSchema=obj({{"path",str()},{"animation",str()},{"x",num()},{"y",num()},{"position",arr()}},{"path","animation"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_blendspace_samples",
        .description="Bulk-replace all samples on a UBlendSpace. `samples` is an array of {animation, x?, y?, position?} entries. Existing samples are removed first (idempotent). Returns {sample_count, added, skipped: [...]}. Each item where `animation` is missing or fails to resolve goes into `skipped`.",
        .inputSchema=obj({{"path",str()},{"samples",arr()}},{"path","samples"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster A (AnimGraph node creation core) ------------

    reg(registry, Tool{.name="animation.add_animgraph_node",
        .description="Spawn a UAnimGraphNode_* subclass node into a target graph (root AnimGraph by default, or a state machine sub-graph via `graph_name`). `node_class` is a UClass path like '/Script/AnimGraph.AnimGraphNode_SequencePlayer'. Returns {node_id, class, graph, x, y}. -32602 if class isn't a UAnimGraphNode_Base subclass or is abstract.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"node_class",str()},{"x",num()},{"y",num()}},{"path","node_class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.remove_animgraph_node",
        .description="Remove an AnimGraph node from a graph by FGuid id (returned by add_animgraph_node / list_animgraph_nodes). Disconnects all pins. Returns {removed: true}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"node_id",str()}},{"path","node_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.connect_pose_pin",
        .description="Connect an output pose pin on `from_node_id` to an input pose pin on `to_node_id` inside the target graph. Pin names default to first-output-pose / first-input-pose if `from_pin` / `to_pin` are omitted (handles common case). Use FGuid digits as ids. Returns {connected: true}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"from_node_id",str()},{"to_node_id",str()},{"from_pin",str()},{"to_pin",str()}},{"path","from_node_id","to_node_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.disconnect_pose_pin",
        .description="Break the link between two pose pins (mirror of connect_pose_pin args). Returns {disconnected: true}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"from_node_id",str()},{"to_node_id",str()},{"from_pin",str()},{"to_pin",str()}},{"path","from_node_id","to_node_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_anim_node_property",
        .description="Mutate a single property on the inner FAnimNode_* struct of a UAnimGraphNode_*. Looks up `property` first on the wrapper UObject (rare display flags), then descends into the `Node` UPROPERTY struct. JSON value coerces: string→FName/object path/FString/enum-by-name, number→float/int/bool, array→FVector/FRotator/FLinearColor by length. Returns {set: true}. -32602 if property unknown or value can't coerce.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"node_id",str()},{"property",str()},{"value",nlohmann::json::object()}},{"path","node_id","property","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.bind_anim_node_property",
        .description="DEPRECATED — current build returns -32601. UE 5.7 marked UAnimGraphNode_Base::PropertyBindings as PropertyBindings_DEPRECATED; the canonical replacement uses the UAnimBlueprintExtension subsystem and is pending Sage implementation. Until then, set node properties via animation.set_anim_node_property and accept the literal-value semantics.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"node_id",str()},{"property",str()},{"variable",str()}},{"path","node_id","property","variable"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.list_animgraph_nodes",
        .description="List every node in a target graph. Returns {graph, count, nodes: [{node_id, class, x, y, pin_count, binding_count?}]}. binding_count present only for UAnimGraphNode_Base derivatives.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.set_animgraph_root_pose",
        .description="Wire a node's first-output-pose pin to the AnimGraph Root node's first-input-pose (the canonical 'Output Pose' in the root AnimGraph or any state's BoundGraph). Works for both UAnimGraphNode_Root (root AnimGraph) and UAnimGraphNode_StateResult (state BoundGraph). Replaces whatever currently feeds root. Returns {connected: true, root_id}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"node_id",str()}},{"path","node_id"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster B (AnimGraph convenience nodes) ------------

    reg(registry, Tool{.name="animation.add_sequence_player",
        .description="Spawn a UAnimGraphNode_SequencePlayer in the target graph and (optionally) bind its `Sequence` property to a UAnimSequenceBase asset. Returns {node_id, class, sequence?, loop, rate}. For non-default loop/rate use animation.set_anim_node_property after.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"sequence",str()},{"x",num()},{"y",num()},{"loop",bln()},{"rate",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_blendspace_player",
        .description="Spawn a UAnimGraphNode_BlendSpacePlayer and bind its asset to a UBlendSpace path. Returns {node_id, class, blendspace?}. Bind axis variables via animation.bind_anim_node_property when that's wired (currently DEPRECATED — see bind tool).",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"blendspace",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_state_machine_node",
        .description="Spawn a UAnimGraphNode_StateMachine that references an existing state machine sub-graph by name (must be created via animation.create_state_machine first). Useful for putting a single SM under a BlendListByBool or behind a state. -32602 if the named SM doesn't exist. Returns {node_id, class, state_machine_name}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"state_machine_name",str()},{"x",num()},{"y",num()}},{"path","state_machine_name"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_blend_list_by_bool",
        .description="Spawn a UAnimGraphNode_BlendListByBool. After spawn, set the `bActiveValue` (input bool pin's literal default) via animation.set_anim_node_property and connect its True/False pose inputs. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_blend_list_by_enum",
        .description="Spawn a UAnimGraphNode_BlendListByEnum. Set the `BoundEnum` UEnum class via animation.set_anim_node_property after spawn. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_layered_blend_per_bone",
        .description="Spawn a UAnimGraphNode_LayeredBoneBlend (upper/lower body split, etc.). Set the BlendPoseList / BoneFilters via animation.set_anim_node_property after spawn. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_apply_additive",
        .description="Spawn a UAnimGraphNode_ApplyAdditive (additive layering). Returns {node_id, class}. Connect base pose + additive pose inputs separately via animation.connect_pose_pin.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_two_bone_ik",
        .description="Spawn a UAnimGraphNode_TwoBoneIK (foot IK / arm IK). Set IKBone / EffectorLocation / JointTargetLocation via animation.set_anim_node_property. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_skeletal_control_node",
        .description="Generic spawn for any UAnimGraphNode_SkeletalControlBase derivative (CopyBone, ModifyBone, RotationMultiplier, FabrikIK, etc.). Pass full UClass path via `control_class`. -32602 if class isn't a SkeletalControlBase subclass. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"control_class",str()},{"x",num()},{"y",num()}},{"path","control_class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_play_montage_notify_window",
        .description="Spawn a UAnimGraphNode_Slot (montage slot — anim-graph integration of montage playback). Set SlotName via animation.set_anim_node_property. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="animation.add_link_anim_layer",
        .description="Spawn a UAnimGraphNode_LinkedAnimLayer (calls into a layer interface function on a runtime-linked AnimInstance). Set Interface + LayerFunctionName via animation.set_anim_node_property after. Returns {node_id, class}.",
        .inputSchema=obj({{"path",str()},{"graph_name",str()},{"x",num()},{"y",num()}},{"path"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster J (runtime character.* — PIE-only) ----------

    reg(registry, Tool{.name="character.play_root_motion_source",
        .description="Apply a FRootMotionSource_* to an ACharacter's UCharacterMovementComponent (Lyra Sage Gap #19 — predicted, replicated movement push). `source_type`: 'ConstantForce' (linear over duration) | 'JumpForce' (vertical impulse) | 'RadialForce' (`location` + `radius` define influence sphere) | 'MoveToForce' (`target_location` + optional `start_location`). `direction` is a [x,y,z] vector (will be normalized for ConstantForce). `accumulate_mode`: 'Override'|'Additive'. `finish_velocity_mode`: 'MaintainLastRootMotion'|'SetVelocity'|'ClampVelocity'. JumpForce optional curves: `path_offset_curve` (UCurveVector path), `time_mapping_curve` (UCurveFloat path). `sensitive_liftoff_check` (default true) for ConstantForce + JumpForce. PIE-only. Returns {source_id (uint16), duration, strength}.",
        .inputSchema=obj({{"actor",str()},{"source_type",str()},{"direction",arr()},{"strength",num()},{"duration",num()},{"accumulate_mode",str()},{"finish_velocity_mode",str()},{"debug_name",str()},{"location",arr()},{"radius",num()},{"target_location",arr()},{"start_location",arr()},{"path_offset_curve",str()},{"time_mapping_curve",str()},{"sensitive_liftoff_check",bln()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.remove_root_motion_source",
        .description="Cancel a running root-motion source by ID on a Character's CMC. PIE-only.",
        .inputSchema=obj({{"actor",str()},{"source_id",num()}},{"actor","source_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.play_montage",
        .description="Play a UAnimMontage on the actor's AnimInstance (PIE runtime). Optional `start_section` jumps to a named section after start. Optional `start_position` (seconds) calls Montage_SetPosition post-Play. Returns {actor, montage, length, play_rate}. -32603 if Montage_Play returns 0.",
        .inputSchema=obj({{"actor",str()},{"montage",str()},{"play_rate",num()},{"start_section",str()},{"start_position",num()}},{"actor","montage"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.stop_montage",
        .description="Stop montages on the actor's AnimInstance with the supplied blend-out time (default 0.25s). Optional `montage` (UAnimMontage path) stops only that specific montage instead of all. PIE-only. Returns {stopped: true}.",
        .inputSchema=obj({{"actor",str()},{"blend_out_time",num()},{"montage",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.set_anim_instance_class",
        .description="Swap an actor's runtime AnimInstance class (USkeletalMeshComponent::SetAnimInstanceClass). Useful for Lyra-style HeroData layer swaps. PIE-only. -32602 if class isn't a UAnimInstance subclass. Returns {swapped: true}.",
        .inputSchema=obj({{"actor",str()},{"anim_class",str()}},{"actor","anim_class"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.list_active_montages",
        .description="List active FAnimMontageInstance entries on the actor's AnimInstance. PIE-only. Returns {actor, count, montages: [{montage, position, weight, play_rate}]}.",
        .inputSchema=obj({{"actor",str()}},{"actor"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.set_animation_mode",
        .description="Set the SkeletalMeshComponent's animation mode: 'AnimBlueprint' (default), 'AnimAsset' (single asset playback), 'Custom'. PIE-only. Returns {actor, mode}.",
        .inputSchema=obj({{"actor",str()},{"mode",str()}},{"actor","mode"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.play_animation",
        .description="Play a single UAnimationAsset on the actor's SkeletalMeshComponent (side-steps AnimBP — sets AnimationMode to AnimationSingleNode). PIE-only. Returns {animation, looping}.",
        .inputSchema=obj({{"actor",str()},{"animation",str()},{"looping",bln()}},{"actor","animation"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="character.set_morph_target",
        .description="Drive a named morph target on the actor's SkeletalMeshComponent. Value typically [0..1]. PIE-only. Returns {target_name, value}.",
        .inputSchema=obj({{"actor",str()},{"target_name",str()},{"value",num()}},{"actor","target_name","value"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster C ek (state machine deep CRUD) -------------

    reg(registry, Tool{.name="animation.add_conduit",
        .description="Add a UAnimStateConduitNode (transient state with rule expression) to a state machine. Conduits don't play animation — they evaluate transition logic and route. Returns {conduit_id, name}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"name",str()},{"x",num()},{"y",num()}},{"path","state_machine_name","name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_state_alias",
        .description="Add a UAnimStateAliasNode (UE 5.0+) — multi-source transition target representing a group of states. Optional `aliased_states` (FGuid string list of state ids) and `global_alias` (default false). Returns {alias_id, name, global_alias, aliased_count}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"name",str()},{"x",num()},{"y",num()},{"aliased_states",arr()},{"global_alias",bln()}},{"path","state_machine_name","name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_transition_priority",
        .description="Set the integer priority (PriorityOrder) on a transition — lower number = higher priority when multiple transitions are eligible.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"transition_id",str()},{"priority",i32()}},{"path","state_machine_name","transition_id","priority"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_state_machine_initial_state",
        .description="Set the state machine's initial state by re-linking the entry node's output to the supplied state_id.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"state_id",str()}},{"path","state_machine_name","state_id"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.list_states",
        .description="Read every UAnimStateNodeBase in a state machine. Returns {count, states: [{state_id, class, name, x, y}]}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()}},{"path","state_machine_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.list_transitions",
        .description="Read every UAnimStateTransitionNode in a state machine. Returns {count, transitions: [{transition_id, blend_time, priority}]}.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()}},{"path","state_machine_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_transition_rule",
        .description="[NOT IMPLEMENTED] Boolean expression authoring inside transition's BoundGraph.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"transition_id",str()},{"expression",str()},{"source_blueprint_path",str()}},{"path","state_machine_name","transition_id"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_state_entered_event",
        .description="[NOT IMPLEMENTED] State OnEntered/OnExited custom event hook.",
        .inputSchema=obj({{"path",str()},{"state_machine_name",str()},{"state_id",str()},{"custom_event_name",str()}},{"path","state_machine_name","state_id","custom_event_name"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster D ek (notify track CRUD) -------------------

    reg(registry, Tool{.name="animation.add_notify_track",
        .description="Append a new notify track on an AnimSequenceBase (multi-track support beyond default index 0).",
        .inputSchema=obj({{"path",str()},{"track_name",str()}},{"path","track_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.list_notifies",
        .description="List every FAnimNotifyEvent on an AnimSequenceBase. Returns {count, notifies: [{index, name, time, duration, track, class?}]}.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.remove_notify",
        .description="Remove a single notify entry by index (from list_notifies).",
        .inputSchema=obj({{"path",str()},{"index",i32()}},{"path","index"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_notify_position",
        .description="Update a notify entry's time (and optionally duration) by index.",
        .inputSchema=obj({{"path",str()},{"index",i32()},{"time",num()},{"duration",num()}},{"path","index","time"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster E ek (blendspace axis settings) ------------

    reg(registry, Tool{.name="animation.remove_blendspace_sample",
        .description="Delete a single blendspace sample by index. Returns {sample_count, removed}.",
        .inputSchema=obj({{"path",str()},{"index",i32()}},{"path","index"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_blendspace_axis",
        .description="[STUB — pending implementation] Configure a blendspace axis: display name, min/max range, grid divisions. axis: 'X' or 'Y'.",
        .inputSchema=obj({{"path",str()},{"axis",str()},{"name",str()},{"min",num()},{"max",num()},{"grid_divisions",i32()}},{"path","axis"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_blendspace_smoothing",
        .description="[STUB — pending implementation] Set per-axis interpolation time (smoothing) on a blendspace.",
        .inputSchema=obj({{"path",str()},{"axis",str()},{"interpolation_speed",num()}},{"path","axis","interpolation_speed"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_blendspace_target_weight_interpolation",
        .description="[STUB — pending implementation] Set TargetWeightInterpolationSpeedPerSec on a blendspace (sample-weight smoothing).",
        .inputSchema=obj({{"path",str()},{"time",num()}},{"path","time"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.read_blendspace_samples",
        .description="Read every sample on a blendspace. Returns {count, samples: [{animation, x, y}]}.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster F (sync markers + compression + modifier) --

    reg(registry, Tool{.name="animation.add_sync_marker",
        .description="Append an FAnimSyncMarker (named time-tagged sync point) on a UAnimSequence.",
        .inputSchema=obj({{"path",str()},{"marker_name",str()},{"time",num()}},{"path","marker_name","time"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.remove_sync_marker",
        .description="Remove a sync marker by index.",
        .inputSchema=obj({{"path",str()},{"index",i32()}},{"path","index"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.list_sync_markers",
        .description="Read all authored sync markers on a UAnimSequence. Returns {count, markers: [{index, name, time}]}.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_curve_compression",
        .description="[NOT IMPLEMENTED] UAnimCurveCompressionCodec asset reference assignment.",
        .inputSchema=obj({{"path",str()},{"codec",str()}},{"path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.run_animation_modifier",
        .description="[NOT IMPLEMENTED] UAnimationModifier::ApplyToAnimationSequence.",
        .inputSchema=obj({{"path",str()},{"modifier_class_path",str()}},{"path","modifier_class_path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_animation_modifier",
        .description="[NOT IMPLEMENTED] Sequence->AnimationModifier_AddInstance.",
        .inputSchema=obj({{"path",str()},{"modifier_class_path",str()}},{"path","modifier_class_path"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster G (anim layer interface — stubbed) --------

    reg(registry, Tool{.name="animation.create_anim_layer_interface",
        .description="[NOT IMPLEMENTED] UAnimLayerInterface BP factory.",
        .inputSchema=obj({{"path",str()},{"name",str()}},{"path","name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_layer_function",
        .description="[NOT IMPLEMENTED] Anim layer interface function.",
        .inputSchema=obj({{"path",str()},{"function_name",str()}},{"path","function_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.implement_anim_layer_interface",
        .description="[NOT IMPLEMENTED] AnimBP implements layer interface.",
        .inputSchema=obj({{"path",str()},{"layer_interface_path",str()}},{"path","layer_interface_path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_linked_anim_layer",
        .description="[NOT IMPLEMENTED] Runtime SetLinkedAnimLayer on AnimInstance.",
        .inputSchema=obj({{"actor",str()},{"layer_function_name",str()},{"anim_class_path",str()}},{"actor","layer_function_name","anim_class_path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.list_implemented_layers",
        .description="[NOT IMPLEMENTED] List anim layer interface implementations.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster H (sequence/montage advanced) -------------

    reg(registry, Tool{.name="animation.set_sequence_additive_settings",
        .description="Configure a UAnimSequence's additive settings: additive_type ('LocalSpaceBase'|'MeshSpaceBase'|'None'), base_animation reference for ref pose.",
        .inputSchema=obj({{"path",str()},{"additive_type",str()},{"base_pose_type",str()},{"base_animation",str()}},{"path","additive_type"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_sequence_compression_scheme",
        .description="[NOT IMPLEMENTED] BoneCompressionSettings asset binding.",
        .inputSchema=obj({{"path",str()},{"scheme_path",str()}},{"path"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_montage_branching_point",
        .description="Append a branching point marker (cooked-time event tagged with branch name).",
        .inputSchema=obj({{"path",str()},{"branch_name",str()},{"time",num()}},{"path","branch_name","time"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_montage_blend_curve",
        .description="[NOT IMPLEMENTED] BlendIn/Out curve type.",
        .inputSchema=obj({{"path",str()},{"blend_in_or_out",str()},{"curve_path",str()}},{"path","blend_in_or_out"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_montage_section_loop",
        .description="Configure a montage section to loop (NextSectionName == self) or not (NextSectionName cleared).",
        .inputSchema=obj({{"path",str()},{"section_name",str()},{"loop",bln()}},{"path","section_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_montage_section_next",
        .description="Chain section1 → section2 (NextSectionName). Pass empty next_section_name to clear.",
        .inputSchema=obj({{"path",str()},{"section_name",str()},{"next_section_name",str()}},{"path","section_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.copy_animation_curves",
        .description="[NOT IMPLEMENTED] Curve transfer between sequences.",
        .inputSchema=obj({{"from_sequence",str()},{"to_sequence",str()},{"curve_names",arr()}},{"from_sequence","to_sequence"}),
        .handler=nullptr,.remote=true});

    // ----- Phase 4-r6 Cluster I (skeleton authoring) --------------------

    reg(registry, Tool{.name="animation.add_skeleton_socket",
        .description="Add a USkeletalMeshSocket to a USkeleton, attached to parent_bone. Transform optional (defaults to identity at parent bone).",
        .inputSchema=obj({{"path",str()},{"socket_name",str()},{"parent_bone",str()},{"transform",obj({})}},{"path","socket_name","parent_bone"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.remove_skeleton_socket",
        .description="Remove a socket by name.",
        .inputSchema=obj({{"path",str()},{"socket_name",str()}},{"path","socket_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_slot",
        .description="Define an animation slot on a skeleton (binds slot_name to group_name). Slot groups required for montage playback.",
        .inputSchema=obj({{"path",str()},{"slot_name",str()},{"group_name",str()}},{"path","slot_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_slot_group",
        .description="Define a slot group on a skeleton.",
        .inputSchema=obj({{"path",str()},{"group_name",str()}},{"path","group_name"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.set_bone_translation_retargeting",
        .description="DEPRECATED — pending Sage implementation (Animation/Skeleton/AnimationScaled/AnimationRelativeToRefPose/OrientAndScale modes per bone).",
        .inputSchema=obj({{"path",str()},{"bone",str()},{"mode",str()}},{"path","bone","mode"}),
        .handler=nullptr,.remote=true});
    reg(registry, Tool{.name="animation.add_skeleton_curve_metadata",
        .description="DEPRECATED — pending Sage implementation (MaterialCurve / MorphTarget metadata).",
        .inputSchema=obj({{"path",str()},{"curve_name",str()},{"type",str()}},{"path","curve_name","type"}),
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
        .description="Append a (key→action) binding to an InputMappingContext "
                     "asset via UInputMappingContext::MapKey. `key` is an "
                     "FKey name like 'SpaceBar', 'LeftMouseButton', "
                     "'Gamepad_FaceButton_Bottom', 'C', 'One'. Optional "
                     "`triggers` and `modifiers` are string arrays — short "
                     "names (e.g. 'Pressed', 'Hold', 'Tap', 'Pulse', "
                     "'HoldAndRelease', 'ChordAction', 'ChordBlocker', "
                     "'Combo' for triggers; 'Negate', 'DeadZone', 'Scalar', "
                     "'ScaleByDeltaTime', 'FOVScaling', 'ResponseCurve', "
                     "'Smooth', 'SwizzleAxis', 'ToWorldSpace' for modifiers) "
                     "or fully-qualified class paths "
                     "('/Script/EnhancedInput.InputTriggerPressed'). Unknown "
                     "names are dropped silently and reported in "
                     "`skipped_triggers`/`skipped_modifiers`. Returns "
                     "{asset_path, action, key, trigger_count, "
                     "modifier_count, mapping_count, mapping_index, skipped_*?}. "
                     "`mapping_index` is the index of the mapping just appended "
                     "(use for subsequent set_mapping_modifiers / "
                     "remove_imc_mapping calls without re-listing). -32602 if "
                     "asset/action/key is invalid. (Lyra Sage Gap #10 fix.)",
        .inputSchema=obj({
            {"path",str()},{"action",str()},{"key",str()},
            {"triggers",arr()},{"modifiers",arr()},
        },{"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_mapping_modifiers",
        .description="Replace (NOT append) the trigger and modifier chains "
                     "on an existing IMC mapping identified by (action, key). "
                     "Same `triggers`/`modifiers` short-name vocabulary as "
                     "gameplay.add_imc_mapping. Returns {asset_path, action, "
                     "key, index, trigger_count, modifier_count, "
                     "skipped_*?}. -32602 if no mapping matches. (Lyra "
                     "Sage Gap #10 fix.)",
        .inputSchema=obj({
            {"path",str()},{"action",str()},{"key",str()},
            {"triggers",arr()},{"modifiers",arr()},
        },{"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.remove_imc_mapping",
        .description="Remove a key mapping from an InputMappingContext by "
                     "(action, key) via UInputMappingContext::UnmapKey. "
                     "Returns {asset_path, action, key, removed_index, "
                     "mapping_count}. -32602 if no mapping matches. (Lyra "
                     "Sage Gap #10 fix.)",
        .inputSchema=obj({{"path",str()},{"action",str()},{"key",str()}},
                          {"path","action","key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_imc_mapping_key",
        .description="Rebind an existing IMC mapping (matched by action + "
                     "old_key) to new_key. Triggers/modifiers preserved. "
                     "Returns {asset_path, action, old_key, new_key, index}. "
                     "-32602 if no mapping matches old_key. (Lyra Sage Gap "
                     "#10 fix.)",
        .inputSchema=obj({
            {"path",str()},{"action",str()},
            {"old_key",str()},{"new_key",str()},
        },{"path","action","old_key","new_key"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="gameplay.set_imc_mapping_action",
        .description="Retarget an existing IMC mapping (matched by old_action "
                     "+ key) to new_action. Triggers/modifiers preserved. "
                     "Returns {asset_path, old_action, new_action, key, "
                     "index}. -32602 if no mapping matches. (Lyra Sage Gap "
                     "#10 fix.)",
        .inputSchema=obj({
            {"path",str()},{"old_action",str()},{"new_action",str()},{"key",str()},
        },{"path","old_action","new_action","key"}),
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
        .description="Override the default GameMode class in World Settings (TSubclassOf<AGameModeBase>). Destructive — pass `confirmed:true` to proceed.",
        .inputSchema=obj({{"game_mode_class",str()},{"confirmed",bln()}},{"game_mode_class","confirmed"}),
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
        .description="Load a level asset (replaces persistent level). Rejects during PIE. If the current world is dirty, rejects -32602 unless `discard_unsaved:true` is set (mirrors editor's 'Save before opening?' dialog).",
        .inputSchema=obj({{"path",str()},{"discard_unsaved",bln()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.list",
        .description="List UWorld assets in a directory.",
        .inputSchema=obj({{"path",str()},{"max_results",i32()}}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="level.create",
        .description="Create a new level asset via UWorld::CreateWorld pipeline (registers FXSystem/Niagara). Destructive — writes a new UWorld asset. Pass `confirmed:true` to proceed.",
        .inputSchema=obj({{"path",str()},{"confirmed",bln()}},{"path","confirmed"}),
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
        .inputSchema=obj({{"path",str()},{"destination",str()}},{"path","destination"}),
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
        .description="Rebuild a material from a previously exported JSON node+edge spec. Symmetric round-trip with mat.export_graph (top-level `nodes` array).",
        .inputSchema=obj({{"path",str()},{"nodes",arr()}},{"path","nodes"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.build_graph",
        .description="Create a material from a declarative spec (PBR helper) in one call. Flat shape: `base_color` (RGB array), `metallic` (0..1), `roughness` (0..1), `emissive` (RGB array).",
        .inputSchema=obj({{"path",str()},{"base_color",arr()},{"metallic",num()},{"roughness",num()},{"emissive",arr()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.render_preview",
        .description="Render a material preview PNG via SceneCapture2D. Returns base64 PNG or note about headless limitation.",
        .inputSchema=obj({{"path",str()},{"resolution",i32()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.begin_transaction",
        .description="Open an FScopedTransaction for a sequence of material graph edits. `path` keys the per-material transaction (ADR-017 safe). `description` shows in the editor's Undo/Redo history.",
        .inputSchema=obj({{"path",str()},{"description",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="mat.end_transaction",
        .description="Commit the open material editing transaction. Optional `path` selects the per-material transaction; omit to commit the global slot.",
        .inputSchema=obj({{"path",str()}}),
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
        .description="Write or overwrite a .h/.cpp/.inl/.cs/.Build.cs/.Target.cs file under the project Source/ directory. path must resolve under FPaths::ProjectDir() (engine source writes rejected). INI/.uproject/.uplugin are explicitly rejected — use project.set_config / project.set_plugin_enabled. Destructive — pass `confirmed:true` to proceed.",
        .inputSchema=obj({{"path",str()},{"content",str()},{"confirmed",bln()}},{"path","content","confirmed"}),
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
        .description="Create an EditorUtilityWidget Blueprint asset (UEditorUtilityWidgetBlueprint). asset is a UEditorUtilityWidgetBlueprint — required form for Editor Utility menu and widget.run_utility_widget.",
        .inputSchema=obj({{"path",str()}},{"path"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="widget.run_utility_widget",
        .description="Spawns the editor utility widget as a registered nomad tab via UEditorUtilitySubsystem. Requires the asset to be a UEditorUtilityWidgetBlueprint (created by widget.create_utility_widget).",
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
        .inputSchema=obj({{"path",str()},{"widget_name",str()},{"new_parent",str()}},{"path","widget_name","new_parent"}),
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
    // bp.override_inherited_component_class — Lyra Sage Gap #12
    // ========================================================================

    reg(registry, Tool{.name="bp.override_inherited_component_class",
        .description="Override the *class* of a SCS component inherited from "
                     "a parent Blueprint. Storage: UBlueprint::"
                     "ComponentClassOverrides (TArray<FBPComponentClassOverride>"
                     "{ComponentName,ComponentClass}). On compile the BP "
                     "regenerates the component template using the override "
                     "class. TopDownArena B_Hero_Arena → CharMoveComp class "
                     "swap is the canonical UE pattern. `component` is the "
                     "FName matching the parent's SCS node; `new_class` must "
                     "be a TopLevelAssetPath to a UClass derived from the "
                     "parent's SCS component class. `recompile` (default "
                     "true) drives FKismetEditorUtilities::CompileBlueprint "
                     "after the edit. Idempotent: re-running with the same "
                     "args updates the existing entry instead of duplicating. "
                     "Returns {blueprint, component, old_class, new_class, "
                     "recompiled, created}. -32602 if the BP/parent SCS node/"
                     "class doesn't resolve or the new class isn't a subclass "
                     "of the parent's. (Lyra Sage Gap #12 fix.)",
        .inputSchema=obj({
            {"path",str()},
            {"component",str()},
            {"new_class",str()},
            {"recompile",bln()},
        },{"path","component","new_class"}),
        .handler=nullptr,.remote=true});

    // ========================================================================
    // asset.add_array_element — Lyra Sage Gap #11
    // ========================================================================

    reg(registry, Tool{.name="asset.add_array_element",
        .description="Append a single element to a UPROPERTY TArray on an "
                     "asset's CDO. Pairs with Sage's recursive FStructProperty "
                     "writer so DataAsset struct arrays (FLyraInputAction[], "
                     "FLyraAbilitySet_GameplayAbility[], etc.) can be authored "
                     "incrementally. `element_value` shape matches the inner "
                     "property — string for object/path refs, JSON object "
                     "{field:value,...} for structs, scalars for primitives. "
                     "Optional `class_name` switches to the instanced "
                     "UObject branch: NewObject<class>() then applies "
                     "element_value as the subobject's property dict (covers "
                     "TArray<UGameFeatureAction*> and similar instanced "
                     "patterns). Returns {asset_path, array_property, index, "
                     "length}. -32602 if asset/property is missing or the "
                     "inner type rejects the value.",
        .inputSchema=obj({
            {"asset_path",str()},
            {"array_property",str()},
            {"element_value",nlohmann::json::object()},
            {"class_name",str()},
            {"allow_empty",bln()},
        },{"asset_path","array_property","element_value"}),
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
                     "to 10000 (range 1..200000). Returns optional "
                     "`_perf_warning` when include_actor_props=true and "
                     "many actors are returned; consider simplify=stripped.",
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
                     "returns the absolute path). When set, output_path resolves "
                     "under the project directory; absolute paths and `..` "
                     "escapes outside the project are rejected. Defaults: "
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
        .description="Add a keyframe to the first section of a named track. Auto-creates the section if absent. Resolves channel by type fallback: float → double → integer → bool at the given index. `track_name` is the UMovieSceneTrack object name (use the `name` field from seq.list_tracks). `value` is numeric for float/double/integer, boolean for bool channels. Returns {track_name, frame, channel_index, channel_type}.",
        .inputSchema=obj({{"path",str()},{"track_name",str()},{"time",num()},{"value",{{"description","JSON value"}}},{"channel_index",i32()}},{"path","track_name","time","value"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="seq.add_possessable",
        .description="Add a possessable binding (existing level actor) to a Level Sequence.",
        .inputSchema=obj({{"path",str()},{"actor_id",str()}},{"path","actor_id"}),
        .handler=nullptr,.remote=true});

    reg(registry, Tool{.name="seq.add_spawnable",
        .description="Add a spawnable binding (template object) to a Level Sequence. Returns {guid} for subsequent binding targeting.",
        .inputSchema=obj({{"path",str()},{"class_path",str()}},{"path","class_path"}),
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
