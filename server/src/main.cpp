// Sage server entry point.
//
// Wires:
//   ToolRegistry   ← registerBuiltins (ping)
//   MCPServer      ← registry
//   HTTP+SSE       ← MCPServer  (Claude ↔ server, port 7777)
//   BridgeServer   ← editor sessions (plugin ↔ server WebSocket, port 7778)
//
// Listens until SIGINT / SIGTERM, then graceful shutdown.

#include "bridge/bridge_server.h"
#include "graph/asset_indexer.h"
#include "graph/cypher_subset.h"
#include "graph/graph_store_manager.h"
#include "mcp/server.h"
#include "mcp/tool_registry.h"
#include "tools/builtin.h"
#include "tools/restart_orchestrator.h"
#include "transport/http_sse_server.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

std::atomic<sage::transport::HttpSseServer*> g_runningHttp{nullptr};
std::atomic<sage::bridge::BridgeServer*>     g_runningBridge{nullptr};

extern "C" void signalHandler(int signal) {
    // Async-signal-safe surface: only atomics + library stop() functions
    // documented as safe from signal context (cpp-httplib, ixwebsocket).
    if (auto* h = g_runningHttp.load(std::memory_order_acquire)) {
        h->stop();
    }
    if (auto* b = g_runningBridge.load(std::memory_order_acquire)) {
        b->stop();
    }
    spdlog::info("Caught signal {}; shutdown initiated", signal);
}

[[nodiscard]] std::string envOr(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string{v} : std::move(fallback);
}

[[nodiscard]] int envIntOr(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    try {
        return std::stoi(std::string{v});
    } catch (const std::exception&) {
        spdlog::warn("Env {}={} not a valid integer; using default {}", name, v, fallback);
        return fallback;
    }
}

}  // namespace

int main() {
    auto logger = spdlog::stdout_color_mt("sage");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    const auto levelStr = envOr("SAGE_LOG_LEVEL", "info");
    spdlog::set_level(spdlog::level::from_str(levelStr));

    spdlog::info("sage-server starting (version 0.1.0, log_level={})", levelStr);

    auto registry = std::make_shared<sage::mcp::ToolRegistry>();
    sage::tools::registerBuiltins(*registry);
    spdlog::info("Registered {} built-in tool(s)", registry->size());

    sage::mcp::MCPServer mcpServer(
        sage::mcp::ServerInfo{.name = "sage-unreal-mcp", .version = "0.1.0"},
        registry);

    // ---- Bridge (plugin ↔ server WebSocket) -----------------------------
    sage::bridge::BridgeConfig bridgeCfg{
        .host     = envOr("SAGE_WS_HOST", "127.0.0.1"),
        .port     = envIntOr("SAGE_WS_PORT", 7778),
        .endpoint = "/bridge",
    };
    sage::bridge::BridgeServer bridge(bridgeCfg);
    if (!bridge.start()) {
        spdlog::error("Bridge failed to bind {}:{}; aborting",
                      bridgeCfg.host, bridgeCfg.port);
        return 1;
    }
    g_runningBridge.store(&bridge, std::memory_order_release);

    // ---- Wire bridge into tool registry (remote tools route via WS) -----
    registry->setRemoteDispatcher(
        [&bridge](std::string_view tool, const nlohmann::json& args) {
            return bridge.dispatchTool(tool, args);
        });

    // ---- Knowledge layer (Phase 2) --------------------------------------
    const auto sageDataDir = []() -> std::filesystem::path {
        if (const char* d = std::getenv("SAGE_DATA_DIR")) return std::filesystem::path{d};
        if (const char* h = std::getenv("HOME"))          return std::filesystem::path{h} / ".sage-mcp";
        return std::filesystem::temp_directory_path() / "sage-mcp";
    }();
    const auto graphRoot = sageDataDir / "graph";
    auto graphMgr = std::make_shared<sage::graph::GraphStoreManager>(graphRoot);
    spdlog::info("Knowledge graph root: {}", graphRoot.string());

    // Resolves an explicit slot_id from params, else falls back to the
    // active session, else (when exactly one editor connected) that one.
    auto resolveSlotId = [&bridge](const nlohmann::json& params)
        -> std::expected<std::string, sage::mcp::ErrorObject> {
        if (params.is_object() && params.contains("slot_id")
            && params["slot_id"].is_string()) {
            return params["slot_id"].get<std::string>();
        }
        const auto activeId = bridge.activeSession();
        if (!activeId.empty()) {
            if (auto s = bridge.snapshotSession(activeId)) return s->slot_id;
        }
        const auto sessions = bridge.snapshotSessions();
        if (sessions.size() == 1) return sessions.front().slot_id;
        return std::unexpected(sage::mcp::ErrorObject::fromCode(
            sage::mcp::ErrorCode::EditorNotConnected,
            "no slot_id provided and no unique active editor; "
            "either pass slot_id or call set_active_editor first"));
    };

    // Smoke-test remote tool: round-trips through the connected editor.
    {
        sage::mcp::Tool editorPing{
            .name        = "editor.ping",
            .description = "Round-trips a ping through the connected editor; "
                           "smoke test for bridge dispatch.",
            .inputSchema = nlohmann::json{
                {"type",       "object"},
                {"properties", {{"message", {{"type", "string"}}}}},
                {"additionalProperties", false},
            },
            .handler = nullptr,
            .remote  = true,
        };
        if (auto r = registry->registerTool(std::move(editorPing)); !r.has_value()) {
            spdlog::warn("Failed to register remote tool 'editor.ping'");
        }
    }

    // Actor mutation tools (Milestone 1.3c).
    auto registerRemote = [&registry](sage::mcp::Tool tool) {
        const auto name = tool.name;
        if (auto r = registry->registerTool(std::move(tool)); !r.has_value()) {
            spdlog::warn("Failed to register remote tool '{}'", name);
        }
    };

    registerRemote(sage::mcp::Tool{
        .name        = "spawn_actor",
        .description = "Spawn an actor in the current editor world. Wrapped in "
                       "FScopedTransaction (undo-friendly). Rejects during PIE "
                       "(api-spec.md §Error Codes -32004). 'class' accepts engine "
                       "paths (/Script/Engine.StaticMeshActor) or Blueprint "
                       "generated-class paths (/Game/.../BP_Foo.BP_Foo_C).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class",    {{"type", "string"},
                              {"description", "UClass path or BP generated-class path"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"label",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "delete_actor",
        .description = "Destroy an actor by full path. FScopedTransaction wrapped. "
                       "Rejects during PIE (-32004).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"},
                              {"description", "Full UE path returned by spawn_actor"}}},
            }},
            {"required", nlohmann::json::array({"actor_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "set_transform",
        .description = "Update an actor's transform. At least one of "
                       "location/rotation/scale must be provided. "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"rotation", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
                {"scale",    {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"required", nlohmann::json::array({"actor_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "set_visibility",
        .description = "Toggle an actor's editor + game visibility "
                       "(SetActorHiddenInGame + SetIsTemporarilyHiddenInEditor). "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"hidden",   {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "hidden"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "modify_actor_property",
        .description = "Set a UProperty on an actor by name. Phase 1 supports "
                       "primitive types (bool, int, int64, float, double, string, "
                       "name, text, byte). Calls PreEditChange/PostEditChange so "
                       "editor notifications fire. Wrapped in FScopedTransaction. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id", {{"type", "string"}}},
                {"property", {{"type", "string"},
                              {"description", "UProperty name as declared in C++ (e.g. 'bHidden', 'CustomTimeDilation')"}}},
                {"value",    {{"description", "JSON-encoded value matching the property type"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // Component tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "add_component",
        .description = "Attach a new UActorComponent to an actor by class. "
                       "RegisterComponent + AddInstanceComponent under "
                       "FScopedTransaction. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_id",        {{"type", "string"}}},
                {"component_class", {{"type", "string"},
                                     {"description", "UClass path (e.g. /Script/Engine.StaticMeshComponent)"}}},
                {"component_name",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"actor_id", "component_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "remove_component",
        .description = "Destroy an instance component by full path. "
                       "FScopedTransaction wrapped. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"component_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"component_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "modify_component_property",
        .description = "Set a UProperty on a component by name. Same primitive "
                       "set as modify_actor_property. PreEditChange/"
                       "PostEditChange + FScopedTransaction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"component_id", {{"type", "string"}}},
                {"property",     {{"type", "string"}}},
                {"value",        {{"description", "JSON value matching property type"}}},
            }},
            {"required", nlohmann::json::array({"component_id", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "attach",
        .description = "Attach a USceneComponent child to a USceneComponent "
                       "parent (KeepRelativeTransform). Optional 'socket' name. "
                       "FScopedTransaction. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"child_id",  {{"type", "string"}}},
                {"parent_id", {{"type", "string"}}},
                {"socket",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"child_id", "parent_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "detach",
        .description = "Detach a USceneComponent from its parent "
                       "(KeepRelativeTransform). FScopedTransaction. Rejects "
                       "during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"child_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"child_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // Asset tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "modify_asset_property",
        .description = "Set a UProperty on a content-browser asset by path. "
                       "Same primitive types as modify_actor_property. "
                       "MarkPackageDirty + FScopedTransaction. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"},
                                {"description", "Asset path like /Game/MyFolder/MyAsset"}}},
                {"property",   {{"type", "string"}}},
                {"value",      {{"description", "JSON value matching property type"}}},
            }},
            {"required", nlohmann::json::array({"asset_path", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "rename_asset",
        .description = "Rename an asset within the same folder (UEditorAsset"
                       "Subsystem::RenameAsset). FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "move_asset",
        .description = "Move an asset to a different folder (RenameAsset under "
                       "the hood; semantic alias of rename_asset for cross-folder "
                       "moves). FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "duplicate_asset",
        .description = "Duplicate an asset to a new path. Returns new asset's "
                       "UE path in `new_asset_id`. FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"source",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"source", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "delete_asset",
        .description = "Delete an asset by path (UEditorAssetSubsystem::"
                       "DeleteAsset). FScopedTransaction. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "save_assets",
        .description = "Save dirty content + world packages. With `paths` array: "
                       "save those specific assets. Without paths: save all dirty. "
                       "`dry_run: true` returns the would-save list without writing. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths",   {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"dry_run", {{"type", "boolean"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "get_dirty_assets",
        .description = "List currently-dirty content + world packages "
                       "(in-memory edits not yet saved to disk). Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {}},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "discard_changes",
        .description = "Reload packages from disk, discarding in-memory edits. "
                       "Wraps UEditorLoadingAndSavingUtils::ReloadPackages with "
                       "AssumeNegative interaction mode (no UI prompt). Rejects "
                       "during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}}},
            }},
            {"required", nlohmann::json::array({"paths"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // ---- Multi-editor MCP-side query/select (Milestone 1.5a) -----------
    // Local tools — no plugin dispatch needed; query the bridge state.
    auto sessionToJson = [](const sage::bridge::EditorSession& s) {
        return nlohmann::json{
            {"session_id",     s.session_id},
            {"slot_id",        s.slot_id},
            {"label",          s.label},
            {"instance_id",    s.instance_id},
            {"project_id",     s.project_id},
            {"project_path",   s.project_path},
            {"engine_version", s.engine_version},
            {"pid",            s.pid},
        };
    };

    sage::mcp::Tool listEditorsTool{
        .name        = "list_editors",
        .description = "List connected editor sessions (slot, label, instance, "
                       "project, engine version, pid).",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json&) -> sage::mcp::ToolResult {
            const auto sessions = bridge.snapshotSessions();
            nlohmann::json items = nlohmann::json::array();
            for (const auto& s : sessions) items.push_back(sessionToJson(s));
            return nlohmann::json{{"editors", items}, {"count", items.size()}};
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(listEditorsTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'list_editors'");
    }

    sage::mcp::Tool getActiveEditorTool{
        .name        = "get_active_editor",
        .description = "Return the active-editor pointer (server-wide). "
                       "If no active session is set, returns the only connected "
                       "session when there is exactly one, otherwise null.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json&) -> sage::mcp::ToolResult {
            const auto activeId = bridge.activeSession();
            if (!activeId.empty()) {
                if (auto s = bridge.snapshotSession(activeId)) {
                    return sessionToJson(*s);
                }
            }
            // Fallback: if exactly one session connected, return it.
            const auto sessions = bridge.snapshotSessions();
            if (sessions.size() == 1) return sessionToJson(sessions.front());
            return nlohmann::json{{"active", nullptr}, {"connected_count", sessions.size()}};
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(getActiveEditorTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'get_active_editor'");
    }

    sage::mcp::Tool setActiveEditorTool{
        .name        = "set_active_editor",
        .description = "Set the active-editor pointer by session_id or label "
                       "(or instance_id). Returns the resolved editor on success.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"id_or_label", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"id_or_label"})},
            {"additionalProperties", false},
        },
        .handler = [&bridge, sessionToJson](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("id_or_label")
                || !params["id_or_label"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'id_or_label' string"));
            }
            const auto idOrLabel = params["id_or_label"].get<std::string>();
            auto session = bridge.findByIdOrLabel(idOrLabel);
            if (!session.has_value()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::EditorNotConnected,
                    std::string{"no editor matches: "} + idOrLabel));
            }
            bridge.setActiveSession(session->session_id);
            return sessionToJson(*session);
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(setActiveEditorTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'set_active_editor'");
    }

    // ---- Editor state + selection tools (Milestone 1.3c) ---------------
    auto noArgSchema = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"additionalProperties", false},
    };

    registerRemote(sage::mcp::Tool{
        .name        = "get_world",
        .description = "Return the current editor world: path, map name, and "
                       "current-level actor count. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_pie_state",
        .description = "Whether PIE is active; if so, returns the play-world path. "
                       "Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_viewport_state",
        .description = "Return basic active viewport metrics (size). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_selected_actors",
        .description = "Return the currently selected actors as a list of UE "
                       "paths. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "select_actors",
        .description = "Replace the editor selection with the given actor paths. "
                       "Returns selected + not_found arrays.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"actor_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"actor_ids"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "clear_selection",
        .description = "Clear the editor's current actor selection.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Level tools (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "save_level",
        .description = "Save the current editor world's level package "
                       "(UEditorLoadingAndSavingUtils::SavePackages on the world's "
                       "outermost package). Rejects during PIE.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_current_level",
        .description = "Return current world's level path, map name, actor count, "
                       "and streaming sub-level package names. Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // QA / Automation Framework (Milestone 1.7).
    registerRemote(sage::mcp::Tool{
        .name        = "list_tests",
        .description = "List available Automation Framework tests in Editor "
                       "context. Optional 'filter' substring narrows results.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"filter", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "run_tests",
        .description = "Trigger Automation Framework tests in Editor context. "
                       "Filter substring narrows the set; tests run async — the "
                       "tool returns the started list immediately. Result polling "
                       "is deferred to Phase 2.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"filter", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Source control (Milestone 1.7).
    registerRemote(sage::mcp::Tool{
        .name        = "get_source_control_state",
        .description = "Source control module loaded/enabled state and active "
                       "provider name (Perforce, Git, Subversion, ...). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "checkout_files",
        .description = "Check out files via the active source control provider. "
                       "paths: array of file or asset paths. Returns provider, "
                       "file count, and status (succeeded/failed/cancelled). "
                       "-32005 if SCM disabled or unavailable.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"minItems", 1}}},
            }},
            {"required", nlohmann::json::array({"paths"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Compile coordination (Milestone 1.6a — Live Coding wrapper).
    registerRemote(sage::mcp::Tool{
        .name        = "get_live_coding_status",
        .description = "Live Coding availability + state. Cross-platform: returns "
                       "{available:true, ...flags} on Windows when LC module loaded; "
                       "{available:false, reason, platform} elsewhere (UE 5.7's "
                       "Live Coding is Windows-only).",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "compile_and_reload",
        .description = "Trigger Live Coding compile (Windows). Auto-enables for "
                       "session if possible. Async fire-and-forget; poll "
                       "get_live_coding_status to observe completion. Returns "
                       "-32007 LiveCodingUnavailable on macOS/Linux. Full-restart "
                       "orchestration (save→shutdown→UBT→relaunch) deferred to "
                       "Phase 2 per ADR-009.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Optimistic locking (Milestone 1.4c).
    registerRemote(sage::mcp::Tool{
        .name        = "compare_and_set_property",
        .description = "Atomic compare-and-set on a primitive UProperty across "
                       "actor / component / asset targets. Returns -32003 "
                       "VersionConflict if current != expected (with current "
                       "and expected echoed in error.data). On success applies "
                       "value within FScopedTransaction. Same primitive set as "
                       "modify_*_property.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"target_kind", {{"type", "string"},
                                 {"enum", nlohmann::json::array({"actor","component","asset"})}}},
                {"target_id",   {{"type", "string"},
                                 {"description", "UE path; for asset use /Game/.../AssetName"}}},
                {"property",    {{"type", "string"}}},
                {"expected",    {{"description", "Current value the agent expects"}}},
                {"new_value",   {{"description", "Value to write if expected matches"}}},
            }},
            {"required", nlohmann::json::array(
                {"target_kind","target_id","property","expected","new_value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // Multi-step transactions (Milestone 1.4b).
    registerRemote(sage::mcp::Tool{
        .name        = "begin_transaction",
        .description = "Open a new editor transaction. All subsequent mutations "
                       "apply within it (their inner FScopedTransaction nests). "
                       "Returns tx_id for commit/rollback. UTransactor is LIFO; "
                       "balance commit/rollback in reverse order of begin.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"label", {{"type", "string"}, {"description", "Display name for Edit > Undo"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "commit_transaction",
        .description = "Finalize an open transaction (GEditor->EndTransaction). "
                       "tx_id must be the most recently opened tx not yet "
                       "committed/rolled-back.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"tx_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"tx_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "rollback_transaction",
        .description = "Cancel an open transaction (GEditor->CancelTransaction). "
                       "Reverts every mutation captured since begin_transaction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"tx_id", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"tx_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "get_active_transactions",
        .description = "List currently-open Sage transactions (tx_id + UTransactor index). Read-only.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Bulk modify (Milestone 1.4a — atomic-by-default multi-op).
    registerRemote(sage::mcp::Tool{
        .name        = "bulk_modify",
        .description = "Apply a sequence of tool calls. atomic=true (default): "
                       "all operations in one FScopedTransaction; first failure "
                       "cancels the transaction (atomic). atomic=false: each op "
                       "runs independently. operations: [{tool, args?}]. "
                       "`bulk_modify` cannot be nested. Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"operations", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool", {{"type", "string"}}},
                            {"args", {{"type", "object"}}},
                        }},
                        {"required", nlohmann::json::array({"tool"})},
                    }},
                }},
                {"atomic", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"operations"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // PIE control (Milestone 1.3c → spec'te 1.7'de listelenmişti, hot path).
    registerRemote(sage::mcp::Tool{
        .name        = "run_pie",
        .description = "Start Play-In-Editor with default parameters (PIE in "
                       "selected viewport). Errors if PIE already active.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name        = "stop_pie",
        .description = "Request end of the active PIE session "
                       "(GEditor->RequestEndPlayMap). Errors if PIE not active.",
        .inputSchema = noArgSchema,
        .handler = nullptr, .remote = true,
    });

    // Material parameter (Milestone 1.3c).
    registerRemote(sage::mcp::Tool{
        .name        = "modify_material_parameter",
        .description = "Set a scalar (number) or vector (3/4-element array → "
                       "FLinearColor) parameter on a UMaterialInstanceConstant. "
                       "Backed by UMaterialEditingLibrary. FScopedTransaction. "
                       "Rejects during PIE.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path", {{"type", "string"},
                                {"description", "Material instance constant path"}}},
                {"parameter",  {{"type", "string"}}},
                {"value",      {{"description", "Number for scalar; 3-4 element array for vector (RGBA)"}}},
            }},
            {"required", nlohmann::json::array({"asset_path", "parameter", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr,
        .remote  = true,
    });

    // ---- Knowledge layer MCP tools (Milestone 2.2 — T1 indexing) -------
    sage::mcp::Tool indexSlotTool{
        .name        = "index_slot",
        .description = "Reindex the slot's knowledge graph from the editor's "
                       "AssetRegistry. Server asks the connected plugin for a "
                       "full asset scan, then full-replaces the Asset table. "
                       "If 'slot_id' omitted, uses the active editor (or the "
                       "single connected editor when exactly one is present). "
                       "Returns {slot_id, asset_count, last_indexed_at_ms, "
                       "scan_ms?}. -32001 EditorNotConnected if no editor "
                       "available, -32603 InternalError on plugin or DB failure.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"slot_id", {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [&bridge, graphMgr, resolveSlotId](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            // Plugin scans AssetRegistry; we receive {assets, scan_ms}.
            auto scan = bridge.dispatchTool(
                "_scan_asset_registry", nlohmann::json::object());
            if (!scan.has_value()) return std::unexpected(scan.error());

            const auto& payload = scan.value();
            if (!payload.contains("assets") || !payload["assets"].is_array()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    "plugin response missing 'assets' array"));
            }

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                // payload already has the {assets, dependencies?} shape that
                // ingestSnapshot expects.
                auto ingest = sage::graph::ingestSnapshot(store, payload);
                if (sage::graph::is_error(ingest)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        "ingest failed: " + sage::graph::error_of(ingest).message));
                }
                nlohmann::json out = sage::graph::value_of(ingest);
                out["slot_id"] = *slot;
                if (payload.contains("scan_ms")) out["scan_ms"] = payload["scan_ms"];
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"graph slot acquire failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(indexSlotTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'index_slot'");
    }

    sage::mcp::Tool indexStatusTool{
        .name        = "index_status",
        .description = "Read the slot's last index pass: {slot_id, asset_count, "
                       "last_indexed_at_ms}. last_indexed_at_ms is null if the "
                       "slot has never been indexed. Read-only — does not open "
                       "an editor connection.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"slot_id", {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());
            try {
                auto& store = graphMgr->acquireSlot(*slot);
                auto st = sage::graph::getIndexStatus(store);
                if (sage::graph::is_error(st)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(st).message));
                }
                nlohmann::json out = sage::graph::value_of(st);
                out["slot_id"] = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"graph slot acquire failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(indexStatusTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'index_status'");
    }

    // ---- Knowledge layer query tools (Milestone 2.4) -------------------
    // Cypher escape helper (kept inline to avoid pulling sage-graph internals
    // into the public surface; refactored into a util header in 2.5).
    auto escCypher = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\\')      out.append("\\\\");
            else if (c == '\'') out.append("\\'");
            else                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };

    sage::mcp::Tool impactOfTool{
        .name        = "impact_of",
        .description = "Reverse-traversal of DEPENDS_ON. Returns assets that "
                       "would be affected if `asset_path` changed — i.e. "
                       "transitive referencers up to `max_depth` hops "
                       "(default 3, range 1..10). Result is a deduplicated "
                       "list ordered by path; `truncated:true` indicates the "
                       "limit was hit. Use this before edits as the safety "
                       "check for delete/rename/refactor.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path",  {{"type", "string"},
                                 {"description", "/Game/.../Asset.Asset (SoftObjectPath form)"}}},
                {"max_depth",   {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 10}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("asset_path")
                || !params["asset_path"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'asset_path'"));
            }
            const auto path = params["asset_path"].get<std::string>();
            const int  maxDepth   = std::clamp(params.value("max_depth",   3), 1, 10);
            const int  maxResults = std::clamp(params.value("max_results", 100), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (target:Asset {path: " << escCypher(path)
                  << "})<-[:DEPENDS_ON*1.." << maxDepth << "]-(impacted:Asset) "
                  << "RETURN DISTINCT impacted.path AS path, impacted.kind AS kind "
                  << "ORDER BY path LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["target"]    = path;
                out["max_depth"] = maxDepth;
                out["impacted"]  = env["rows"];
                out["count"]     = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"impact_of failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(impactOfTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'impact_of'");
    }

    sage::mcp::Tool referencesToTool{
        .name        = "references_to",
        .description = "Direct (1-hop) inbound references. Returns the assets "
                       "that explicitly DEPENDS_ON `asset_path` — equivalent "
                       "to impact_of with max_depth=1 but cheaper. Use for "
                       "'who imports this' surveys; use impact_of for safety "
                       "before destructive edits.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"asset_path",  {{"type", "string"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"asset_path"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("asset_path")
                || !params["asset_path"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'asset_path'"));
            }
            const auto path = params["asset_path"].get<std::string>();
            const int  maxResults = std::clamp(params.value("max_results", 100), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (target:Asset {path: " << escCypher(path)
                  << "})<-[:DEPENDS_ON]-(ref:Asset) "
                  << "RETURN ref.path AS path, ref.kind AS kind "
                  << "ORDER BY path LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["target"]     = path;
                out["references"] = env["rows"];
                out["count"]      = env["row_count"];
                out["truncated"]  = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]    = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"references_to failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(referencesToTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'references_to'");
    }

    sage::mcp::Tool findUnusedTool{
        .name        = "find_unused",
        .description = "Assets with no incoming DEPENDS_ON edge — candidates "
                       "for cleanup. CAVEAT: only catches asset-to-asset "
                       "references tracked by AssetRegistry. C++ code "
                       "references (e.g. UClass::FindObject('/Game/...')), "
                       "config files, and runtime-string lookups are NOT "
                       "in the graph; never auto-delete from this list — "
                       "treat it as a triage view, not a kill list.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"kind",        {{"type", "string"},
                                 {"description", "Optional: filter by asset kind (e.g. 'Texture2D')"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            const int maxResults = std::clamp(params.value("max_results", 100), 1, 500);
            std::optional<std::string> kindFilter;
            if (params.is_object() && params.contains("kind") && params["kind"].is_string()) {
                kindFilter = params["kind"].get<std::string>();
            }

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                q << "MATCH (a:Asset) WHERE NOT EXISTS { MATCH (a)<-[:DEPENDS_ON]-() }";
                if (kindFilter) q << " AND a.kind = " << escCypher(*kindFilter);
                q << " RETURN a.path AS path, a.kind AS kind ORDER BY path LIMIT "
                  << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["unused"]    = env["rows"];
                out["count"]     = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                if (kindFilter) out["kind"] = *kindFilter;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"find_unused failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(findUnusedTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'find_unused'");
    }

    // ---- Cypher subset escape hatch (Milestone 2.5) --------------------
    sage::mcp::Tool queryGraphTool{
        .name        = "query_graph",
        .description = "Read-only Cypher against the slot's knowledge graph. "
                       "Use when impact_of / references_to / find_unused don't "
                       "fit the question. Banned: CREATE, MERGE, SET, DELETE, "
                       "DETACH, REMOVE, DROP, ALTER, COPY, LOAD, INSERT, CALL "
                       "(read-only — for writes use ingest tools / direct MCP "
                       "tools). Variable-length traversals must be bounded "
                       "*N..M with M ≤ 10. LIMIT is auto-injected if missing "
                       "(default 200, max 1000).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"cypher",      {{"type", "string"},
                                 {"description", "Cypher MATCH/RETURN query (max 8KB)"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 1000}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"cypher"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("cypher")
                || !params["cypher"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'cypher' string"));
            }
            const auto cypher = params["cypher"].get<std::string>();
            const int maxResults = std::clamp(params.value("max_results",
                sage::graph::kDefaultRowLimit), 1, sage::graph::kMaxRowLimit);

            // Whitelist + bound check.
            const auto v = sage::graph::validateReadOnlySubset(cypher);
            if (!v.ok) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams,
                    "cypher rejected: " + v.error));
            }

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            const auto bounded = sage::graph::ensureLimit(cypher, maxResults);
            try {
                auto& store = graphMgr->acquireSlot(*slot);
                auto r = store.execute(bounded);
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InvalidParams,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["rows"]      = env["rows"];
                out["schema"]    = env["schema"];
                out["row_count"] = env["row_count"];
                out["truncated"] = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]   = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"query_graph failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(queryGraphTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'query_graph'");
    }

    // ---- Editor restart orchestrator (Milestone 1.6b) ------------------
    sage::tools::RestartConfig restartCfg{
        .repoRoot = envOr("SAGE_REPO_ROOT", std::filesystem::current_path().string()),
        .ueRoot   = envOr("SAGE_UE_ROOT",   ""),
    };
    spdlog::info("Restart orchestrator: repo_root={} ue_root={}",
                 restartCfg.repoRoot.string(),
                 restartCfg.ueRoot.empty() ? "<from-script-default>" : restartCfg.ueRoot.string());
    if (auto r = registry->registerTool(
            sage::tools::buildRestartEditorTool(bridge, std::move(restartCfg)));
        !r.has_value()) {
        spdlog::warn("Failed to register 'restart_editor'");
    }

    // ---- Real-time delta (Milestone 2.3b) -------------------------------
    // Plugin emits AssetRegistry deltas as `event` envelopes; we patch the
    // per-slot graph in place so the snapshot stays current without a full
    // re-index. Failures are logged but never rethrown — async stream.
    bridge.setEventHandler(
        [graphMgr, escCypher](std::string_view slot_id,
                              const sage::bridge::EventMessage& ev) {
            try {
                auto& store = graphMgr->acquireSlot(slot_id);
                const auto& p = ev.payload;

                if (ev.kind == "asset_added") {
                    if (!p.contains("path") || !p.contains("kind")) return;
                    std::ostringstream q;
                    q << "MERGE (a:Asset {path: "
                      << escCypher(p["path"].get<std::string>())
                      << "}) SET a.kind = "
                      << escCypher(p["kind"].get<std::string>()) << ";";
                    auto r = store.execute(q.str());
                    if (sage::graph::is_error(r)) {
                        spdlog::warn("delta asset_added failed: {}",
                                     sage::graph::error_of(r).message);
                    }
                }
                else if (ev.kind == "asset_removed") {
                    if (!p.contains("path")) return;
                    std::ostringstream q;
                    q << "MATCH (a:Asset {path: "
                      << escCypher(p["path"].get<std::string>())
                      << "}) DETACH DELETE a;";
                    auto r = store.execute(q.str());
                    if (sage::graph::is_error(r)) {
                        spdlog::warn("delta asset_removed failed: {}",
                                     sage::graph::error_of(r).message);
                    }
                }
                else if (ev.kind == "asset_renamed") {
                    if (!p.contains("old_path") || !p.contains("new_path")) return;
                    // Kuzu allows updating the PK column via SET; edges
                    // attached to the node move with it (verified via smoke).
                    std::ostringstream q;
                    q << "MATCH (a:Asset {path: "
                      << escCypher(p["old_path"].get<std::string>())
                      << "}) SET a.path = "
                      << escCypher(p["new_path"].get<std::string>()) << ";";
                    auto r = store.execute(q.str());
                    if (sage::graph::is_error(r)) {
                        spdlog::warn("delta asset_renamed failed: {}",
                                     sage::graph::error_of(r).message);
                    }
                }
                else {
                    spdlog::debug("delta: unhandled event kind='{}'", ev.kind);
                }
            } catch (const std::exception& ex) {
                spdlog::warn("delta handler threw on kind='{}' slot='{}': {}",
                             ev.kind, slot_id, ex.what());
            }
        });

    // ---- HTTP+SSE transport (Claude ↔ server) ---------------------------
    sage::transport::HttpSseConfig httpCfg{
        .host            = envOr("SAGE_HTTP_HOST", "127.0.0.1"),
        .port            = envIntOr("SAGE_HTTP_PORT", 7777),
        .mcpEndpoint     = "/mcp",
        .readTimeoutSec  = 30,
        .writeTimeoutSec = 30,
    };
    sage::transport::HttpSseServer transport(mcpServer, httpCfg);
    g_runningHttp.store(&transport, std::memory_order_release);

    std::signal(SIGINT,  &signalHandler);
    std::signal(SIGTERM, &signalHandler);

    const bool ok = transport.listen();

    g_runningHttp.store(nullptr, std::memory_order_release);
    bridge.stop();
    g_runningBridge.store(nullptr, std::memory_order_release);

    if (!ok) {
        spdlog::error("HTTP server failed to bind {}:{}", httpCfg.host, httpCfg.port);
        return 1;
    }
    spdlog::info("Bye");
    return 0;
}
