#include "mcp/error_codes.h"
#include "mcp/tool.h"
#include "mcp/tool_registry.h"
#include "tools/phase4_schemas.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

using namespace sage::mcp;

namespace {

Tool makeNoopTool(std::string name) {
    return Tool{
        .name        = std::move(name),
        .description = "noop",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json&) -> ToolResult {
            return nlohmann::json::object();
        },
    };
}

}  // namespace

TEST_CASE("ToolRegistry registers and dispatches a tool", "[mcp][registry]") {
    ToolRegistry reg;

    Tool ping{
        .name        = "ping",
        .description = "test ping",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json& p) -> ToolResult {
            nlohmann::json out = {{"pong", true}};
            if (p.is_object() && p.contains("echo")) out["echo"] = p["echo"];
            return out;
        },
    };

    REQUIRE(reg.registerTool(std::move(ping)).has_value());
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.has("ping"));

    auto outcome = reg.dispatch("ping", nlohmann::json{{"echo", "hi"}});
    REQUIRE(outcome.has_value());
    REQUIRE((*outcome)["pong"] == true);
    REQUIRE((*outcome)["echo"] == "hi");
}

TEST_CASE("ToolRegistry rejects duplicate registration", "[mcp][registry]") {
    ToolRegistry reg;
    REQUIRE(reg.registerTool(makeNoopTool("x")).has_value());

    auto err = reg.registerTool(makeNoopTool("x"));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error() == ToolRegistry::RegisterError::DuplicateName);
}

TEST_CASE("ToolRegistry rejects invalid name", "[mcp][registry]") {
    ToolRegistry reg;

    SECTION("empty name") {
        auto err = reg.registerTool(makeNoopTool(""));
        REQUIRE_FALSE(err.has_value());
        REQUIRE(err.error() == ToolRegistry::RegisterError::InvalidName);
    }
    SECTION("disallowed character") {
        auto err = reg.registerTool(makeNoopTool("bad name"));
        REQUIRE_FALSE(err.has_value());
        REQUIRE(err.error() == ToolRegistry::RegisterError::InvalidName);
    }
}

TEST_CASE("ToolRegistry::dispatch returns MethodNotFound for unknown tool", "[mcp][registry]") {
    ToolRegistry reg;
    auto outcome = reg.dispatch("unknown", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::MethodNotFound);
}

TEST_CASE("ToolRegistry::dispatch traps handler exceptions", "[mcp][registry]") {
    ToolRegistry reg;
    Tool throwing{
        .name        = "boom",
        .description = "throws",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json&) -> ToolResult {
            throw std::runtime_error("kaboom");
        },
    };
    REQUIRE(reg.registerTool(std::move(throwing)).has_value());

    auto outcome = reg.dispatch("boom", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}

TEST_CASE("ToolRegistry::list snapshots all tools", "[mcp][registry]") {
    ToolRegistry reg;
    REQUIRE(reg.registerTool(makeNoopTool("a")).has_value());
    REQUIRE(reg.registerTool(makeNoopTool("b")).has_value());
    REQUIRE(reg.registerTool(makeNoopTool("c")).has_value());

    auto items = reg.list();
    REQUIRE(items.size() == 3);
}

TEST_CASE("ToolRegistry rejects local tool with no handler", "[mcp][registry]") {
    ToolRegistry reg;
    Tool broken{
        .name        = "broken",
        .description = "missing handler",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = false,
    };
    auto err = reg.registerTool(std::move(broken));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error() == ToolRegistry::RegisterError::MissingHandler);
}

TEST_CASE("ToolRegistry routes remote tool via dispatcher", "[mcp][registry]") {
    ToolRegistry reg;

    Tool remote{
        .name        = "editor.ping",
        .description = "remote",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());
    REQUIRE_FALSE(reg.hasRemoteDispatcher());

    bool called = false;
    reg.setRemoteDispatcher(
        [&called](std::string_view tool, const nlohmann::json& args,
                  std::string_view /*targetEditor*/) -> ToolResult {
            called = true;
            REQUIRE(tool == "editor.ping");
            return nlohmann::json{{"echoed", args}};
        });
    REQUIRE(reg.hasRemoteDispatcher());

    auto outcome = reg.dispatch("editor.ping", nlohmann::json{{"message", "hi"}});
    REQUIRE(called);
    REQUIRE(outcome.has_value());
    REQUIRE((*outcome)["echoed"]["message"] == "hi");
}

TEST_CASE("ToolRegistry annotates remote tools with async job schema",
          "[mcp][registry]") {
    ToolRegistry reg;
    Tool remote{
        .name        = "asset.long_op",
        .description = "remote",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler     = nullptr,
        .remote      = true,
    };

    REQUIRE(reg.registerTool(std::move(remote)).has_value());
    auto tools = reg.list();
    REQUIRE(tools.size() == 1);
    const auto& schema = tools[0].inputSchema;
    REQUIRE(schema["properties"].contains("path"));
    REQUIRE(schema["properties"].contains("async"));
    REQUIRE(schema["properties"].contains("job_timeout_seconds"));
    REQUIRE(schema["properties"]["async"]["type"] == "boolean");
    REQUIRE(schema["properties"]["job_timeout_seconds"]["maximum"] == 86400);
    REQUIRE(schema["additionalProperties"] == false);
}

TEST_CASE("ToolRegistry remote without dispatcher yields InternalError",
          "[mcp][registry]") {
    ToolRegistry reg;
    Tool remote{
        .name        = "rt",
        .description = "remote",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());

    auto outcome = reg.dispatch("rt", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}

TEST_CASE("ToolRegistry remote dispatcher exception -> InternalError",
          "[mcp][registry]") {
    ToolRegistry reg;
    Tool remote{
        .name        = "rt",
        .description = "throws",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());
    reg.setRemoteDispatcher(
        [](std::string_view, const nlohmann::json&, std::string_view) -> ToolResult {
            throw std::runtime_error("dispatcher boom");
        });

    auto outcome = reg.dispatch("rt", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}

TEST_CASE("Phase4 animation graph gap tools expose compact schemas",
          "[tools][phase4][animation]") {
    ToolRegistry reg;
    sage::tools::registerPhase4Schemas(reg);

    const auto tools = reg.list();
    auto findTool = [&tools](const std::string& name) -> const Tool* {
        for (const auto& tool : tools) {
            if (tool.name == name) return &tool;
        }
        return nullptr;
    };

    const Tool* readStateGraph = findTool("animation.read_state_graph");
    REQUIRE(readStateGraph != nullptr);
    REQUIRE(readStateGraph->inputSchema["properties"].contains("state_machine_name"));
    REQUIRE(readStateGraph->inputSchema["properties"].contains("state_name"));
    REQUIRE(readStateGraph->inputSchema["properties"].contains("state_id"));
    REQUIRE(readStateGraph->inputSchema["properties"].contains("include_properties"));
    REQUIRE(readStateGraph->inputSchema["properties"].contains("include_connections"));
    REQUIRE(readStateGraph->inputSchema["properties"].contains("asset_substring"));
    REQUIRE(readStateGraph->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* readAnimGraph = findTool("animation.read_anim_graph");
    REQUIRE(readAnimGraph != nullptr);
    REQUIRE(readAnimGraph->inputSchema["properties"].contains("graph_name"));
    REQUIRE(readAnimGraph->inputSchema["properties"].contains("node_ids"));

    const Tool* removeRef = findTool("animation.remove_state_machine_reference_node");
    REQUIRE(removeRef != nullptr);
    REQUIRE(removeRef->inputSchema["properties"].contains("dry_run"));
    REQUIRE(removeRef->inputSchema["required"] == nlohmann::json::array({"path", "node_id"}));

    const Tool* removeNode = findTool("animation.remove_animgraph_node");
    REQUIRE(removeNode != nullptr);
    REQUIRE(removeNode->inputSchema["properties"].contains("dry_run"));

    const Tool* layeredBlend = findTool("animation.set_layered_bone_blend_config");
    REQUIRE(layeredBlend != nullptr);
    REQUIRE(layeredBlend->inputSchema["properties"].contains("blend_masks"));
    REQUIRE(layeredBlend->inputSchema["properties"].contains("layer_setup"));
    REQUIRE(layeredBlend->inputSchema["properties"].contains("compile"));
    REQUIRE(layeredBlend->inputSchema["required"] == nlohmann::json::array({"path", "node_id"}));

    const Tool* addWidgetEntry = findTool("gamefeature.add_widget_entry");
    REQUIRE(addWidgetEntry != nullptr);
    REQUIRE(addWidgetEntry->inputSchema["properties"].contains("slot_id"));
    REQUIRE(addWidgetEntry->inputSchema["properties"].contains("widget_class"));
    REQUIRE(addWidgetEntry->inputSchema["properties"].contains("layer_id"));
    REQUIRE(addWidgetEntry->inputSchema["properties"].contains("layout_class"));
    REQUIRE(addWidgetEntry->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* cdoInstancedArray = findTool("bp.set_cdo_instanced_array_element");
    REQUIRE(cdoInstancedArray != nullptr);
    REQUIRE(cdoInstancedArray->inputSchema["properties"].contains("array_property"));
    REQUIRE(cdoInstancedArray->inputSchema["properties"].contains("class_name"));
    REQUIRE(cdoInstancedArray->inputSchema["properties"].contains("element_value"));
    REQUIRE(cdoInstancedArray->inputSchema["properties"].contains("match_fields"));
    REQUIRE(cdoInstancedArray->inputSchema["properties"].contains("dry_run"));
    REQUIRE(cdoInstancedArray->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* createIKRig = findTool("animation.create_ik_rig");
    REQUIRE(createIKRig != nullptr);
    REQUIRE(createIKRig->inputSchema["properties"].contains("skeletal_mesh"));
    REQUIRE(createIKRig->inputSchema["properties"].contains("retarget_root"));
    REQUIRE(createIKRig->inputSchema["properties"].contains("chains"));
    REQUIRE(createIKRig->inputSchema["properties"].contains("dry_run"));
    REQUIRE(createIKRig->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* setIKRigMesh = findTool("animation.set_ik_rig_skeletal_mesh");
    REQUIRE(setIKRigMesh != nullptr);
    REQUIRE(setIKRigMesh->inputSchema["properties"].contains("skeletal_mesh"));
    REQUIRE(setIKRigMesh->inputSchema["properties"].contains("skeletal_mesh_path"));
    REQUIRE(setIKRigMesh->inputSchema["properties"].contains("dry_run"));
    REQUIRE(setIKRigMesh->inputSchema["properties"].contains("validate_only"));
    REQUIRE(setIKRigMesh->inputSchema["properties"].contains("save"));
    REQUIRE(setIKRigMesh->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* inspectSkeleton = findTool("animation.inspect_skeleton");
    REQUIRE(inspectSkeleton != nullptr);
    REQUIRE(inspectSkeleton->inputSchema["properties"].contains("skeletal_mesh"));
    REQUIRE(inspectSkeleton->inputSchema["properties"].contains("include_ref_pose"));

    const Tool* createBlendMask = findTool("animation.create_blend_mask");
    REQUIRE(createBlendMask != nullptr);
    REQUIRE(createBlendMask->inputSchema["properties"].contains("entries"));
    REQUIRE(createBlendMask->inputSchema["properties"].contains("clear_existing"));
    REQUIRE(createBlendMask->inputSchema["properties"].contains("save"));

    const Tool* readBlendProfiles = findTool("animation.read_blend_profiles");
    REQUIRE(readBlendProfiles != nullptr);
    REQUIRE(readBlendProfiles->inputSchema["properties"].contains("blend_masks_only"));

    const Tool* animationListSockets = findTool("animation.list_sockets");
    REQUIRE(animationListSockets != nullptr);
    REQUIRE(animationListSockets->inputSchema["properties"].contains("owner"));

    const Tool* controlRigRead = findTool("controlrig.read");
    REQUIRE(controlRigRead != nullptr);
    REQUIRE(controlRigRead->inputSchema["properties"].contains("include_controls"));
    REQUIRE(controlRigRead->inputSchema["properties"].contains("include_transforms"));
    REQUIRE(controlRigRead->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* controlRigListControls = findTool("controlrig.list_controls");
    REQUIRE(controlRigListControls != nullptr);
    REQUIRE(controlRigListControls->inputSchema["properties"].contains("name_contains"));

    const Tool* controlRigSetPreviewMesh = findTool("controlrig.set_preview_mesh");
    REQUIRE(controlRigSetPreviewMesh != nullptr);
    REQUIRE(controlRigSetPreviewMesh->inputSchema["properties"].contains("preview_mesh"));
    REQUIRE(controlRigSetPreviewMesh->inputSchema["properties"].contains("skeletal_mesh"));
    REQUIRE(controlRigSetPreviewMesh->inputSchema["properties"].contains("save"));

    const Tool* controlRigSetControlTransform = findTool("controlrig.set_control_transform");
    REQUIRE(controlRigSetControlTransform != nullptr);
    REQUIRE(controlRigSetControlTransform->inputSchema["properties"].contains("space"));
    REQUIRE(controlRigSetControlTransform->inputSchema["properties"].contains("initial"));
    REQUIRE(controlRigSetControlTransform->inputSchema["properties"].contains("compile"));

    const Tool* controlRigAddControl = findTool("controlrig.add_control");
    REQUIRE(controlRigAddControl != nullptr);
    REQUIRE(controlRigAddControl->inputSchema["properties"].contains("control_type"));
    REQUIRE(controlRigAddControl->inputSchema["properties"].contains("offset_transform"));
    REQUIRE(controlRigAddControl->inputSchema["required"] == nlohmann::json::array({"path", "name"}));

    const Tool* controlRigRemoveControl = findTool("controlrig.remove_control");
    REQUIRE(controlRigRemoveControl != nullptr);
    REQUIRE(controlRigRemoveControl->inputSchema["properties"].contains("confirmed"));

    const Tool* setupRetargeterOps = findTool("animation.setup_ik_retargeter_ops");
    REQUIRE(setupRetargeterOps != nullptr);
    REQUIRE(setupRetargeterOps->inputSchema["properties"].contains("assign_ik_rigs"));
    REQUIRE(setupRetargeterOps->inputSchema["properties"].contains("clean_chain_maps"));
    REQUIRE(setupRetargeterOps->inputSchema["properties"].contains("auto_map_type"));
    REQUIRE(setupRetargeterOps->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* createRetargeter = findTool("animation.create_ik_retargeter");
    REQUIRE(createRetargeter != nullptr);
    REQUIRE(createRetargeter->inputSchema["properties"].contains("assign_ops"));
    REQUIRE(createRetargeter->inputSchema["properties"].contains("auto_map_type"));
    REQUIRE(createRetargeter->inputSchema["properties"].contains("save"));

    const Tool* setRetargeterRigs = findTool("animation.set_ik_retargeter_rigs");
    REQUIRE(setRetargeterRigs != nullptr);
    REQUIRE(setRetargeterRigs->inputSchema["properties"].contains("clean_chain_maps"));
    REQUIRE(setRetargeterRigs->inputSchema["properties"].contains("dry_run"));

    const Tool* addRetargeterOp = findTool("animation.add_ik_retargeter_op");
    REQUIRE(addRetargeterOp != nullptr);
    REQUIRE(addRetargeterOp->inputSchema["properties"].contains("op_type"));
    REQUIRE(addRetargeterOp->inputSchema["properties"].contains("parent_op_name"));
    REQUIRE(addRetargeterOp->inputSchema["properties"].contains("run_initial_setup"));

    const Tool* fkChainSettings = findTool("animation.set_ik_retargeter_fk_chain_settings");
    REQUIRE(fkChainSettings != nullptr);
    REQUIRE(fkChainSettings->inputSchema["properties"].contains("rotation_mode"));
    REQUIRE(fkChainSettings->inputSchema["properties"].contains("translation_alpha"));
    REQUIRE(fkChainSettings->inputSchema["properties"].contains("dry_run"));

    const Tool* ikChainSettings = findTool("animation.set_ik_retargeter_ik_chain_settings");
    REQUIRE(ikChainSettings != nullptr);
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("static_offset"));
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("static_rotation_offset"));
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("blend_to_source_translation"));
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("scale_vertical"));
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("extension"));
    REQUIRE(ikChainSettings->inputSchema["properties"].contains("dry_run"));
    REQUIRE(ikChainSettings->inputSchema["required"] == nlohmann::json::array({"path", "target_chain"}));

    const Tool* retargetPose = findTool("animation.set_ik_retargeter_pose");
    REQUIRE(retargetPose != nullptr);
    REQUIRE(retargetPose->inputSchema["properties"].contains("duplicate"));
    REQUIRE(retargetPose->inputSchema["properties"].contains("reset_bones"));
    REQUIRE(retargetPose->inputSchema["properties"].contains("auto_align"));
    REQUIRE(retargetPose->inputSchema["properties"].contains("snap_to_ground"));

    const Tool* chainMapping = findTool("animation.set_ik_retargeter_chain_mapping");
    REQUIRE(chainMapping != nullptr);
    REQUIRE(chainMapping->inputSchema["properties"].contains("dry_run"));
    REQUIRE(chainMapping->inputSchema["properties"].contains("save"));

    const Tool* addRetargetPoseFromMesh = findTool("animation.add_retarget_pose_from_mesh_node");
    REQUIRE(addRetargetPoseFromMesh != nullptr);
    REQUIRE(addRetargetPoseFromMesh->inputSchema["properties"].contains("ik_retargeter"));
    REQUIRE(addRetargetPoseFromMesh->inputSchema["properties"].contains("source_mode"));
    REQUIRE(addRetargetPoseFromMesh->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* setRetargetPoseFromMesh = findTool("animation.set_retarget_pose_from_mesh_node");
    REQUIRE(setRetargetPoseFromMesh != nullptr);
    REQUIRE(setRetargetPoseFromMesh->inputSchema["properties"].contains("expose_source_mesh_pin"));
    REQUIRE(setRetargetPoseFromMesh->inputSchema["properties"].contains("target_pose"));
    REQUIRE(setRetargetPoseFromMesh->inputSchema["required"] == nlohmann::json::array({"path", "node_id"}));

    const Tool* readRetargetProfile = findTool("animation.read_retarget_profile");
    REQUIRE(readRetargetProfile != nullptr);
    REQUIRE(readRetargetProfile->inputSchema["properties"].contains("retargeter"));

    const Tool* copyRetargetProfile = findTool("animation.copy_retarget_profile_from_asset");
    REQUIRE(copyRetargetProfile != nullptr);
    REQUIRE(copyRetargetProfile->inputSchema["properties"].contains("ik_retargeter"));

    const Tool* niagaraPreviewSpawn = findTool("niagara.preview_spawn");
    REQUIRE(niagaraPreviewSpawn != nullptr);
    REQUIRE(niagaraPreviewSpawn->inputSchema["properties"].contains("advance_ticks"));
    REQUIRE(niagaraPreviewSpawn->inputSchema["properties"].contains("parameters"));
    REQUIRE(niagaraPreviewSpawn->inputSchema["required"] == nlohmann::json::array({"path"}));

    const Tool* niagaraAddModule = findTool("niagara.add_module");
    REQUIRE(niagaraAddModule != nullptr);
    REQUIRE(niagaraAddModule->inputSchema["properties"].contains("module_script"));
    REQUIRE(niagaraAddModule->inputSchema["properties"].contains("dry_run"));
    REQUIRE(niagaraAddModule->inputSchema["required"] == nlohmann::json::array({"path", "module_script"}));

    const Tool* niagaraCollectionSetDefault = findTool("niagara.collection.set_default");
    REQUIRE(niagaraCollectionSetDefault != nullptr);
    REQUIRE(niagaraCollectionSetDefault->inputSchema["properties"].contains("value"));
    REQUIRE(niagaraCollectionSetDefault->inputSchema["required"] == nlohmann::json::array({"path", "name", "value"}));

    const Tool* niagaraValidateSystem = findTool("niagara.validate_system");
    REQUIRE(niagaraValidateSystem != nullptr);
    REQUIRE(niagaraValidateSystem->inputSchema["required"] == nlohmann::json::array({"path"}));
}
