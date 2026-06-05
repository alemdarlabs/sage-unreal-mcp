#include "tools/sage_guidance_tools.h"

#include "mcp/error_codes.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace sage::tools {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

[[nodiscard]] std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string{value} : std::string{};
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

[[nodiscard]] fs::path weakAbs(const fs::path& path) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    if (ec) out = fs::absolute(path, ec);
    if (ec) out = path;
    return out.lexically_normal();
}

[[nodiscard]] bool isDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_directory(path, ec);
}

[[nodiscard]] bool isRegularFile(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

[[nodiscard]] bool isUprojectPath(const fs::path& path) {
    return lower(path.extension().string()) == ".uproject";
}

[[nodiscard]] Json noArgSchema() {
    return Json{
        {"type", "object"},
        {"properties", Json::object()},
        {"additionalProperties", false},
    };
}

[[nodiscard]] Json objectSchema(Json properties,
                                Json required = Json::array()) {
    Json schema{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"additionalProperties", false},
    };
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

[[nodiscard]] Json stringSchema(std::string description = {}) {
    Json out{{"type", "string"}};
    if (!description.empty()) out["description"] = std::move(description);
    return out;
}

[[nodiscard]] std::vector<fs::path> uprojectsInDir(const fs::path& dir) {
    std::vector<fs::path> matches;
    std::error_code ec;
    if (!isDirectory(dir)) return matches;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && isUprojectPath(entry.path())) {
            matches.push_back(weakAbs(entry.path()));
        }
    }
    std::ranges::sort(matches);
    return matches;
}

[[nodiscard]] Json readJsonFile(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return Json();
    Json parsed = Json::parse(in, nullptr, false);
    return parsed.is_discarded() ? Json() : parsed;
}

[[nodiscard]] std::string pluginVersionFromDescriptor(const fs::path& descriptorPath) {
    Json descriptor = readJsonFile(descriptorPath);
    if (descriptor.is_object()
        && descriptor.contains("VersionName")
        && descriptor["VersionName"].is_string()) {
        return descriptor["VersionName"].get<std::string>();
    }
    return {};
}

[[nodiscard]] std::optional<std::array<int, 4>> parseVersion(std::string_view version) {
    std::string core{version};
    const std::size_t suffix = core.find_first_of("+-");
    if (suffix != std::string::npos) core.resize(suffix);
    if (core.empty()) return std::nullopt;

    std::array<int, 4> out{0, 0, 0, 0};
    std::size_t partIndex = 0;
    std::size_t start = 0;
    while (start <= core.size()) {
        if (partIndex >= out.size()) return std::nullopt;
        const std::size_t dot = core.find('.', start);
        const std::string part = core.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty()
            || !std::ranges::all_of(part, [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)) != 0;
               })) {
            return std::nullopt;
        }
        try {
            out[partIndex] = std::stoi(part);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        ++partIndex;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return out;
}

[[nodiscard]] int compareVersions(std::string_view left, std::string_view right) {
    const auto a = parseVersion(left);
    const auto b = parseVersion(right);
    if (!a || !b) return 0;
    for (std::size_t i = 0; i < a->size(); ++i) {
        if ((*a)[i] < (*b)[i]) return -1;
        if ((*a)[i] > (*b)[i]) return 1;
    }
    return 0;
}

[[nodiscard]] std::string versionStatus(std::string_view installed,
                                        std::string_view expected) {
    if (installed.empty()) return "missing";
    if (expected.empty()) return "unknown";
    if (installed == expected) return "current";
    const auto left = parseVersion(installed);
    const auto right = parseVersion(expected);
    if (!left || !right) return "unknown";
    const int relation = compareVersions(installed, expected);
    if (relation < 0) return "outdated";
    if (relation > 0) return "newer_than_server";
    return "current";
}

[[nodiscard]] Json discoverProject(const Json& params) {
    fs::path start;
    std::string source = "cwd";
    if (params.is_object()
        && params.contains("start_path")
        && params["start_path"].is_string()
        && !params["start_path"].get<std::string>().empty()) {
        start = params["start_path"].get<std::string>();
        source = "param:start_path";
    } else if (!envValue("SAGE_PROJECT_ROOT").empty()) {
        start = envValue("SAGE_PROJECT_ROOT");
        source = "env:SAGE_PROJECT_ROOT";
    } else {
        start = fs::current_path();
    }

    start = weakAbs(start);
    Json out{
        {"ok", false},
        {"source", source},
        {"start_path", start.generic_string()},
        {"project_path", nullptr},
        {"project_root", nullptr},
        {"reason", nullptr},
        {"matches", Json::array()},
    };

    if (isUprojectPath(start)) {
        if (!isRegularFile(start)) {
            out["reason"] = "uproject_not_found";
            return out;
        }
        out["ok"] = true;
        out["project_path"] = start.generic_string();
        out["project_root"] = weakAbs(start.parent_path()).generic_string();
    } else {
        fs::path current = isDirectory(start) ? start : start.parent_path();
        while (!current.empty()) {
            const auto matches = uprojectsInDir(current);
            if (matches.size() == 1) {
                out["ok"] = true;
                out["project_path"] = matches.front().generic_string();
                out["project_root"] = weakAbs(current).generic_string();
                break;
            }
            if (matches.size() > 1) {
                out["reason"] = "multiple_uproject_files";
                for (const auto& match : matches) out["matches"].push_back(match.generic_string());
                return out;
            }
            const fs::path parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        if (!out.value("ok", false)) out["reason"] = "not_found";
    }

    if (out.value("ok", false)) {
        const fs::path projectRoot = fs::path(out["project_root"].get<std::string>());
        const fs::path descriptor = projectRoot / "Plugins" / "SageBridge" / "SageBridge.uplugin";
        const fs::path sourceDir = projectRoot / "Plugins" / "SageBridge" / "Source";
        const fs::path mcpConfig = projectRoot / ".mcp.json";
        const std::string pluginVersion = pluginVersionFromDescriptor(descriptor);
        out["sagebridge"] = {
            {"plugin_dir", weakAbs(projectRoot / "Plugins" / "SageBridge").generic_string()},
            {"descriptor_path", weakAbs(descriptor).generic_string()},
            {"descriptor_exists", isRegularFile(descriptor)},
            {"source_exists", isDirectory(sourceDir)},
            {"plugin_version", pluginVersion.empty() ? Json(nullptr) : Json(pluginVersion)},
            {"expected_version", std::string{kServerVersion}},
            {"version_status", versionStatus(pluginVersion, kServerVersion)},
        };
        out["mcp_config"] = {
            {"path", weakAbs(mcpConfig).generic_string()},
            {"exists", isRegularFile(mcpConfig)},
        };
    }

    out["next_calls"] = Json::array({
        "sage.status",
        "sage.doctor",
        "sage.capabilities",
        "sage.workflow.suggest"
    });
    return out;
}

[[nodiscard]] Json baseGuide(std::string_view topic) {
    const std::string normalized = lower(topic);
    Json out{
        {"product", "Sage Unreal MCP"},
        {"purpose", "Use live Unreal Editor inspection, source intelligence, AssetRegistry queries, and typed editor tools through MCP."},
        {"default_first_call_sequence", Json::array({
             Json{{"tool", "sage.about"}, {"why", "Identify Sage version, transports, and safety model."}},
             Json{{"tool", "sage.project.discover"}, {"why", "Find the Unreal project and installed SageBridge plugin."}},
             Json{{"tool", "sage.doctor"}, {"why", "Detect missing plugin, stale versions, or disconnected editor state."}},
             Json{{"tool", "sage.capabilities"}, {"why", "Choose the smallest tool family for the user's task."}},
             Json{{"tool", "list_editors"}, {"why", "Confirm a live Unreal Editor bridge before editor tools."}},
        })},
        {"operating_rules", Json::array({
             "Use read-only inspection tools before mutation.",
             "For Blueprint mutation, capture bp.full_dump first.",
             "For destructive tools, require explicit user approval and pass confirmed:true only after approval.",
             "Treat build/test success, asset save success, editor connection, and runtime proof as separate facts.",
             "Pass _editor explicitly when multiple editors are connected.",
             "Prefer typed Sage tools over editor.run_python for normal production workflows.",
        })},
        {"setup_commands", Json::array({
             "npm install -g @alemdarlabs/sage-mcp@latest",
             "sage setup codex",
             "sage doctor <Project.uproject>",
             "sage init <Project.uproject> --codex",
             "sage update <Project.uproject>",
        })},
    };

    if (normalized == "blueprint" || normalized == "bp") {
        out["topic"] = "blueprint";
        out["recommended_sequence"] = Json::array({
            "bp.full_dump",
            "bp.read",
            "bp.validate",
            "targeted bp.* mutation with dry_run when available",
            "bp.compile",
            "save_assets",
            "bp.validate"
        });
    } else if (normalized == "asset") {
        out["topic"] = "asset";
        out["recommended_sequence"] = Json::array({
            "asset.search or asset.list",
            "asset.read_properties",
            "targeted asset.* mutation with dry_run when available",
            "save_assets",
            "asset.validate when relevant"
        });
    } else if (normalized == "animation") {
        out["topic"] = "animation";
        out["recommended_sequence"] = Json::array({
            "animation.list or animation.find_animations",
            "animation.read_anim_blueprint / animation.read_anim_graph",
            "animation.inspect_skeleton or animation.inspect_animation",
            "targeted animation.* mutation",
            "bp.compile or animation.save_animation_asset",
            "save_assets"
        });
    } else if (normalized == "niagara") {
        out["topic"] = "niagara";
        out["recommended_sequence"] = Json::array({
            "niagara.list",
            "niagara.get_info",
            "niagara.validate_system",
            "targeted niagara.* mutation",
            "niagara.preview_spawn when visual/runtime proof is needed",
            "save_assets"
        });
    } else {
        out["topic"] = normalized.empty() ? "general" : normalized;
    }
    return out;
}

[[nodiscard]] Json workflowStep(std::string tool,
                                std::string purpose,
                                bool readOnly,
                                bool requiresEditor = true) {
    return Json{
        {"tool", std::move(tool)},
        {"purpose", std::move(purpose)},
        {"read_only", readOnly},
        {"requires_editor", requiresEditor},
    };
}

[[nodiscard]] Json workflowForIntent(const Json& params) {
    const std::string intent = params.is_object()
        ? lower(params.value("intent", params.value("task", "")))
        : std::string{};

    Json out{
        {"intent", intent},
        {"safety", Json{
            {"read_first", true},
            {"confirmed_required_for_destructive_tools", true},
            {"prefer_dry_run_when_available", true},
        }},
    };

    Json steps = Json::array();
    if (intent.empty()) {
        out["workflow_id"] = "choose_workflow";
        out["available_workflows"] = Json::array({
            "setup_or_repair",
            "project_orientation",
            "blueprint_edit",
            "asset_edit",
            "animation_edit",
            "niagara_edit",
            "compile_or_live_coding",
            "destructive_cleanup"
        });
        out["steps"] = Json::array({
            workflowStep("sage.capabilities", "Inspect tool families and counts.", true, false),
            workflowStep("sage.workflow.suggest", "Call again with a concrete intent.", true, false),
        });
        return out;
    }

    if (contains(intent, "setup") || contains(intent, "install")
        || contains(intent, "doctor") || contains(intent, "update")
        || contains(intent, "version")) {
        out["workflow_id"] = "setup_or_repair";
        steps.push_back(workflowStep("sage.about", "Read Sage version and onboarding entry points.", true, false));
        steps.push_back(workflowStep("sage.project.discover", "Find the current Unreal project.", true, false));
        steps.push_back(workflowStep("sage.doctor", "Return actionable install/version/editor checks.", true, false));
    } else if (contains(intent, "blueprint") || contains(intent, " bp")
               || contains(intent, "widget")) {
        out["workflow_id"] = "blueprint_edit";
        steps.push_back(workflowStep("list_editors", "Verify the Unreal bridge before editor calls.", true));
        steps.push_back(workflowStep("bp.full_dump", "Capture a rollback/readback snapshot before mutation.", true));
        steps.push_back(workflowStep("bp.read", "Read the current Blueprint structure.", true));
        steps.push_back(workflowStep("bp.validate", "Surface compile and graph errors before edits.", true));
        steps.push_back(workflowStep("bp.* targeted mutation", "Apply the smallest specific Blueprint edit tool.", false));
        steps.push_back(workflowStep("bp.compile", "Compile and inspect warnings/errors.", false));
        steps.push_back(workflowStep("save_assets", "Persist only after the readback and compile result are acceptable.", false));
    } else if (contains(intent, "asset") || contains(intent, "material")
               || contains(intent, "texture") || contains(intent, "datatable")) {
        out["workflow_id"] = "asset_edit";
        steps.push_back(workflowStep("asset.search", "Resolve the target asset path.", true));
        steps.push_back(workflowStep("asset.read_properties", "Read current asset properties.", true));
        steps.push_back(workflowStep("asset.* targeted mutation", "Use the narrowest asset mutation tool; dry_run first when supported.", false));
        steps.push_back(workflowStep("get_dirty_assets", "Confirm exactly what became dirty.", true));
        steps.push_back(workflowStep("save_assets", "Save explicit paths, not the whole project, when possible.", false));
    } else if (contains(intent, "animation") || contains(intent, "anim")
               || contains(intent, "retarget") || contains(intent, "control rig")) {
        out["workflow_id"] = "animation_edit";
        steps.push_back(workflowStep("animation.list", "Find candidate animation assets.", true));
        steps.push_back(workflowStep("animation.read_anim_blueprint", "Read AnimBlueprint-level structure when relevant.", true));
        steps.push_back(workflowStep("animation.read_anim_graph", "Read graph details before mutation.", true));
        steps.push_back(workflowStep("animation.* targeted mutation", "Apply the narrow animation/IK/retargeting operation.", false));
        steps.push_back(workflowStep("bp.compile", "Compile AnimBlueprint changes when graph-owned.", false));
        steps.push_back(workflowStep("save_assets", "Persist verified assets.", false));
    } else if (contains(intent, "niagara") || contains(intent, "vfx")
               || contains(intent, "particle")) {
        out["workflow_id"] = "niagara_edit";
        steps.push_back(workflowStep("niagara.list", "Find Niagara systems/emitters.", true));
        steps.push_back(workflowStep("niagara.get_info", "Read system/emitter structure.", true));
        steps.push_back(workflowStep("niagara.validate_system", "Collect validation issues before edits.", true));
        steps.push_back(workflowStep("niagara.* targeted mutation", "Apply focused Niagara edit.", false));
        steps.push_back(workflowStep("niagara.preview_spawn", "Spawn a preview only when visual/runtime proof is needed.", false));
        steps.push_back(workflowStep("save_assets", "Save verified assets.", false));
    } else if (contains(intent, "compile") || contains(intent, "build")
               || contains(intent, "live coding")) {
        out["workflow_id"] = "compile_or_live_coding";
        steps.push_back(workflowStep("get_live_coding_status", "Check whether Live Coding is available and idle.", true));
        steps.push_back(workflowStep("compile_and_reload", "Trigger editor Live Coding compile when appropriate.", false));
        steps.push_back(workflowStep("get_live_coding_status", "Poll/read status after dispatch.", true));
    } else if (contains(intent, "delete") || contains(intent, "remove")
               || contains(intent, "destructive")) {
        out["workflow_id"] = "destructive_cleanup";
        steps.push_back(workflowStep("source control/status tool", "Check source control and dirty state first.", true));
        steps.push_back(workflowStep("bp.full_dump or asset.read_properties", "Capture pre-mutation evidence.", true));
        steps.push_back(workflowStep("delete_asset/delete_actor/bp.delete_*", "Run only after explicit user approval with confirmed:true.", false));
        steps.push_back(workflowStep("get_dirty_assets", "Verify the exact write set.", true));
        steps.push_back(workflowStep("save_assets", "Persist only approved changes.", false));
    } else {
        out["workflow_id"] = "project_orientation";
        steps.push_back(workflowStep("sage.status", "Read project/editor/version status.", true, false));
        steps.push_back(workflowStep("project.get_info", "Read project metadata.", true));
        steps.push_back(workflowStep("asset.search", "Find relevant assets.", true));
        steps.push_back(workflowStep("lookup_docs", "Search project/Sage/engine documentation.", true, false));
        steps.push_back(workflowStep("search_unreal_api", "Inspect C++/UE API source before code-level changes.", true, false));
    }

    out["steps"] = steps;
    out["next"] = "Call the listed read-only tools first, then use the narrowest mutation tool that matches the evidence.";
    return out;
}

[[nodiscard]] Json capabilityCategory(std::string id,
                                      std::string description,
                                      std::vector<std::string> prefixes,
                                      std::vector<std::string> firstTools,
                                      const Json& prefixCounts) {
    std::size_t count = 0;
    for (const std::string& prefix : prefixes) {
        if (prefixCounts.is_object() && prefixCounts.contains(prefix)
            && prefixCounts[prefix].is_number_integer()) {
            const auto value = prefixCounts[prefix].get<std::int64_t>();
            if (value > 0) count += static_cast<std::size_t>(value);
        }
    }
    return Json{
        {"id", std::move(id)},
        {"description", std::move(description)},
        {"tool_prefixes", std::move(prefixes)},
        {"recommended_first_tools", std::move(firstTools)},
        {"registered_tool_count", count},
    };
}

[[nodiscard]] Json capabilities(const SageGuidanceContext& context, const Json& params) {
    const Json summary = context.registrySummary ? context.registrySummary() : Json::object();
    const Json prefixCounts = summary.value("prefix_counts", Json::object());
    Json categories = Json::array({
        capabilityCategory("orientation",
                           "Server, project, editor, version, and onboarding checks.",
                           {"sage", "status", "ping", "list_editors", "wait_for_editor"},
                           {"sage.about", "sage.status", "sage.doctor", "list_editors"},
                           prefixCounts),
        capabilityCategory("source_intelligence",
                           "Local source/docs/ADR/reflection/risk inspection without editor dispatch.",
                           {"source", "decision", "risk", "cppreflect", "network", "pipeline", "search_unreal_api", "lookup_docs"},
                           {"status", "lookup_docs", "search_unreal_api", "get_class_reference"},
                           prefixCounts),
        capabilityCategory("project_config",
                           "Project metadata, modules, config files, plugins, and C++ source reads.",
                           {"project", "get_source_control_state", "checkout_files"},
                           {"project.get_info", "project.list_modules", "project.read_config"},
                           prefixCounts),
        capabilityCategory("editor_world",
                           "World, actors, components, selection, PIE, viewport, tests, and editor automation.",
                           {"editor", "get_world", "get_pie_state", "spawn_actor", "delete_actor", "set_transform"},
                           {"get_world", "get_selected_actors", "get_pie_state"},
                           prefixCounts),
        capabilityCategory("assets",
                           "AssetRegistry search, readback, import/export, sockets, textures, data tables, materials.",
                           {"asset", "material", "save_assets", "get_dirty_assets", "delete_asset"},
                           {"asset.search", "asset.read_properties", "get_dirty_assets"},
                           prefixCounts),
        capabilityCategory("blueprints",
                           "Blueprint structure, variables, components, graph edits, CDO, validation, compile.",
                           {"bp", "widget", "gamefeature"},
                           {"bp.full_dump", "bp.read", "bp.validate"},
                           prefixCounts),
        capabilityCategory("animation",
                           "Skeletons, AnimBlueprints, state machines, IK Rig/Retargeter, Control Rig, Pose Search.",
                           {"animation", "controlrig"},
                           {"animation.list", "animation.read_anim_blueprint", "animation.inspect_skeleton"},
                           prefixCounts),
        capabilityCategory("niagara",
                           "Niagara systems, emitters, modules, renderers, parameters, preview, sim caches.",
                           {"niagara"},
                           {"niagara.list", "niagara.get_info", "niagara.validate_system"},
                           prefixCounts),
        capabilityCategory("jobs",
                           "Detached async job management for long-running remote editor operations.",
                           {"jobs"},
                           {"jobs.list", "jobs.get", "jobs.wait", "jobs.logs"},
                           prefixCounts),
    });

    if (params.is_object() && params.contains("category") && params["category"].is_string()) {
        const std::string wanted = lower(params["category"].get<std::string>());
        Json filtered = Json::array();
        for (const Json& category : categories) {
            if (category.is_object() && lower(category.value("id", "")) == wanted) {
                filtered.push_back(category);
            }
        }
        categories = std::move(filtered);
    }

    return Json{
        {"summary", summary},
        {"categories", categories},
        {"selection_rule", "Choose the narrowest category and call its read-only tools before any mutation."},
    };
}

[[nodiscard]] Json status(const SageGuidanceContext& context, const Json& params) {
    Json project = discoverProject(params);
    Json sessions = context.editorSessions ? context.editorSessions() : Json{{"count", 0}, {"editors", Json::array()}};
    Json versionChecks = Json::array();
    std::size_t mismatchCount = 0;

    if (sessions.contains("editors") && sessions["editors"].is_array()) {
        for (const Json& editor : sessions["editors"]) {
            const std::string pluginVersion = editor.value("plugin_version", "");
            const std::string relation = versionStatus(pluginVersion, context.serverVersion);
            if (relation != "current") ++mismatchCount;
            versionChecks.push_back({
                {"session_id", editor.value("session_id", "")},
                {"project_path", editor.value("project_path", "")},
                {"plugin_version", pluginVersion.empty() ? Json(nullptr) : Json(pluginVersion)},
                {"expected_version", context.serverVersion},
                {"status", relation},
            });
        }
    }

    Json actions = Json::array();
    if (!project.value("ok", false)) {
        actions.push_back("Open Codex inside an Unreal project tree or set SAGE_PROJECT_ROOT.");
    } else if (project.contains("sagebridge")
               && !project["sagebridge"].value("descriptor_exists", false)) {
        actions.push_back(std::string{"Run: sage bootstrap \""}
                          + project.value("project_path", std::string{})
                          + "\"");
    } else if (project.contains("sagebridge")
               && project["sagebridge"].value("version_status", "") != "current") {
        actions.push_back(std::string{"Run: sage update \""}
                          + project.value("project_path", std::string{})
                          + "\"");
    }
    if (sessions.value("count", 0) == 0) {
        actions.push_back("Open the Unreal Editor project with SageBridge enabled, then call wait_for_editor.");
    }
    if (mismatchCount > 0) {
        actions.push_back("Update the project SageBridge plugin or the global Sage npm package so versions match.");
    }

    return Json{
        {"ok", true},
        {"server", Json{
             {"name", "sage-unreal-mcp"},
             {"version", context.serverVersion},
             {"protocol_version", context.protocolVersion},
             {"transport", context.transport},
             {"http_endpoint", context.httpEndpoint},
             {"bridge_endpoint", context.bridgeEndpoint},
             {"cwd", weakAbs(fs::current_path()).generic_string()},
             {"data_dir", envValue("SAGE_DATA_DIR")},
             {"log_level", envValue("SAGE_LOG_LEVEL")},
        }},
        {"env", Json{
             {"SAGE_PROJECT_ROOT", envValue("SAGE_PROJECT_ROOT")},
             {"SAGE_REPO_ROOT", envValue("SAGE_REPO_ROOT")},
             {"SAGE_UE_ROOT", envValue("SAGE_UE_ROOT")},
             {"SAGE_HTTP_PORT", envValue("SAGE_HTTP_PORT")},
             {"SAGE_WS_PORT", envValue("SAGE_WS_PORT")},
        }},
        {"project", project},
        {"editors", sessions},
        {"version_checks", versionChecks},
        {"ready", Json{
             {"source_tools", true},
             {"project_discovered", project.value("ok", false)},
             {"editor_tools", sessions.value("count", 0) > 0},
             {"plugin_version_aligned", mismatchCount == 0},
        }},
        {"suggested_actions", actions},
    };
}

[[nodiscard]] Json doctor(const SageGuidanceContext& context, const Json& params) {
    const Json current = status(context, params);
    Json checks = Json::array();
    auto add = [&checks](std::string id, bool ok, std::string severity,
                         std::string message, std::string fix = {}) {
        Json check{
            {"id", std::move(id)},
            {"ok", ok},
            {"severity", std::move(severity)},
            {"message", std::move(message)},
        };
        if (!fix.empty()) check["fix"] = std::move(fix);
        checks.push_back(std::move(check));
    };

    add("server_running", true, "info",
        std::string{"Sage MCP server is running, version "} + context.serverVersion);

    const Json& project = current["project"];
    const bool projectOk = project.value("ok", false);
    add("project_discovery", projectOk, projectOk ? "info" : "warning",
        projectOk ? "Unreal project discovered." : "No Unreal .uproject discovered from this process context.",
        "Run Codex from a project folder, set SAGE_PROJECT_ROOT, or call sage init <Project.uproject>.");

    if (projectOk && project.contains("sagebridge")) {
        const Json& bridge = project["sagebridge"];
        const bool descriptorOk = bridge.value("descriptor_exists", false);
        const std::string projectPath = project.value("project_path", "");
        add("sagebridge_installed", descriptorOk, descriptorOk ? "info" : "error",
            descriptorOk ? "SageBridge descriptor exists." : "SageBridge is missing from the project.",
            std::string{"sage bootstrap \""} + projectPath + "\"");

        const std::string pluginStatus = bridge.value("version_status", "missing");
        add("sagebridge_version", pluginStatus == "current",
            pluginStatus == "current" ? "info" : "error",
            std::string{"SageBridge version status: "} + pluginStatus,
            pluginStatus == "newer_than_server"
                ? "npm install -g @alemdarlabs/sage-mcp@latest"
                : std::string{"sage update \""} + projectPath + "\"");
    }

    const Json& editors = current["editors"];
    const bool editorConnected = editors.value("count", 0) > 0;
    add("editor_connected", editorConnected, editorConnected ? "info" : "warning",
        editorConnected ? "At least one Unreal Editor bridge is connected."
                        : "No Unreal Editor bridge is connected; editor tools will fail until the project is open.",
        "Open the Unreal project, then call wait_for_editor.");

    bool ok = true;
    for (const Json& check : checks) {
        if (check.value("severity", "") == "error" && !check.value("ok", false)) {
            ok = false;
            break;
        }
    }

    return Json{
        {"ok", ok},
        {"checks", checks},
        {"status", current},
        {"next_calls", Json::array({"sage.capabilities", "sage.workflow.suggest"})},
    };
}

void registerLocal(mcp::ToolRegistry& registry,
                   std::string name,
                   std::string description,
                   Json schema,
                   mcp::ToolHandler handler) {
    mcp::Tool tool{
        .name = std::move(name),
        .description = std::move(description),
        .inputSchema = std::move(schema),
        .handler = std::move(handler),
        .remote = false,
    };
    const std::string toolName = tool.name;
    if (auto result = registry.registerTool(std::move(tool)); !result.has_value()) {
        spdlog::warn("sage-guidance: failed to register '{}'", toolName);
    }
}

}  // namespace

void registerSageGuidanceTools(mcp::ToolRegistry& registry, SageGuidanceContext context) {
    if (context.serverVersion.empty()) context.serverVersion = std::string{kServerVersion};
    if (context.protocolVersion.empty()) context.protocolVersion = std::string{kProtocolVersion};

    registerLocal(registry, "sage.about",
                  "Return Sage product identity, version, safety model, and the first MCP calls an AI agent should make.",
                  noArgSchema(),
                  [context](const Json&) -> mcp::ToolResult {
                      return Json{
                          {"name", "Sage Unreal MCP"},
                          {"server_version", context.serverVersion},
                          {"protocol_version", context.protocolVersion},
                          {"transport", context.transport},
                          {"endpoints", Json{
                               {"http", context.httpEndpoint},
                               {"bridge", context.bridgeEndpoint},
                          }},
                          {"what_to_do_first", Json::array({
                               "sage.project.discover",
                               "sage.doctor",
                               "sage.capabilities",
                               "sage.workflow.suggest",
                               "list_editors"
                          })},
                          {"safety_model", Json::array({
                               "Read before write.",
                               "Use dry_run when available.",
                               "Require explicit approval before confirmed:true destructive calls.",
                               "Separate edit, save, build, server health, and runtime proof in reports."
                          })},
                          {"package_commands", Json::array({
                               "npm install -g @alemdarlabs/sage-mcp@latest",
                               "sage setup codex",
                               "sage doctor <Project.uproject>",
                               "sage init <Project.uproject> --codex",
                               "sage update <Project.uproject>"
                          })},
                      };
                  });

    registerLocal(registry, "sage.status",
                  "Return Sage server, project discovery, connected editor, and SageBridge version-alignment status.",
                  objectSchema({
                      {"start_path", stringSchema("Optional file or directory to use for Unreal project discovery.")},
                  }),
                  [context](const Json& params) -> mcp::ToolResult {
                      return status(context, params);
                  });

    registerLocal(registry, "sage.doctor",
                  "Return actionable MCP-side checks for project discovery, SageBridge install/version state, and editor connectivity.",
                  objectSchema({
                      {"start_path", stringSchema("Optional file or directory to use for Unreal project discovery.")},
                  }),
                  [context](const Json& params) -> mcp::ToolResult {
                      return doctor(context, params);
                  });

    registerLocal(registry, "sage.project.discover",
                  "Discover the Unreal .uproject from start_path, SAGE_PROJECT_ROOT, or cwd and report SageBridge install/version state.",
                  objectSchema({
                      {"start_path", stringSchema("Optional file or directory to search upward from.")},
                  }),
                  [](const Json& params) -> mcp::ToolResult {
                      return discoverProject(params);
                  });

    registerLocal(registry, "sage.capabilities",
                  "Return Sage tool families, grouped counts, recommended first tools, and category-selection guidance.",
                  objectSchema({
                      {"category", stringSchema("Optional category id, e.g. blueprint, assets, animation, niagara.")},
                  }),
                  [context](const Json& params) -> mcp::ToolResult {
                      return capabilities(context, params);
                  });

    registerLocal(registry, "sage.workflow.suggest",
                  "Suggest a safe Sage tool sequence for a natural-language intent such as Blueprint edit, Niagara inspect, setup, or destructive cleanup.",
                  objectSchema({
                      {"intent", stringSchema("Natural-language task intent.")},
                      {"task", stringSchema("Alias for intent.")},
                  }),
                  [](const Json& params) -> mcp::ToolResult {
                      return workflowForIntent(params);
                  });

    auto helpHandler = [](const Json& params) -> mcp::ToolResult {
        std::string topic;
        if (params.is_object() && params.contains("topic") && params["topic"].is_string()) {
            topic = params["topic"].get<std::string>();
        }
        return baseGuide(topic);
    };
    const Json helpSchema = objectSchema({
        {"topic", stringSchema("Optional guide topic: general, blueprint, asset, animation, or niagara.")},
    });
    registerLocal(registry, "sage.help",
                  "Return a compact operating guide for AI agents using Sage through MCP.",
                  helpSchema, helpHandler);
    registerLocal(registry, "sage.guide",
                  "Alias of sage.help for MCP clients that look for a guide-style onboarding tool.",
                  helpSchema, helpHandler);
}

}  // namespace sage::tools
