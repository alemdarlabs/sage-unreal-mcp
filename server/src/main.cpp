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

    // ---- Reflection (Phase 4.1) ----------------------------------------
    registerRemote(sage::mcp::Tool{
        .name        = "reflect_class",
        .description = "Full UClass dump: name, parent, module, native/abstract/"
                       "interface flags, interfaces implemented, all UProperties "
                       "(type+category+access+replication+tooltip), all UFunctions "
                       "(parameters+return+access+pure+network), and immediate "
                       "child classes. Accepts engine path (/Script/Engine.Pawn) "
                       "or BP generated-class path (/Game/.../BP_Foo.BP_Foo_C). "
                       "Pair with `class_hierarchy` for transitive walks.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class_path",       {{"type", "string"}}},
                {"include_children", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"class_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "reflect_struct",
        .description = "USTRUCT dump: name, module, parent, all fields with full "
                       "type + flags + tooltip. Accepts /Script/CoreUObject.Vector "
                       "or any USTRUCT path.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"struct_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"struct_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "reflect_enum",
        .description = "UENUM dump: name, module, cpp form, all entries (name + "
                       "value + display name + tooltip). Synthetic _MAX entry "
                       "skipped.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"enum_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"enum_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_classes",
        .description = "Walk the live UClass registry. Filters: substring on name, "
                       "base_class (only subclasses of), include_native, "
                       "include_blueprint. SKEL_/REINST_/HOTRELOADED_ churn "
                       "filtered out. Default 500 max, capped at 5000. Returns "
                       "{name, path, module, is_native, parent}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",            {{"type", "string"}}},
                {"base_class",        {{"type", "string"}}},
                {"include_native",    {{"type", "boolean"}}},
                {"include_blueprint", {{"type", "boolean"}}},
                {"max_results",       {{"type", "integer"},
                                       {"minimum", 1}, {"maximum", 5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_structs",
        .description = "Walk the live UScriptStruct registry. Substring filter, "
                       "max_results 1..5000.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",      {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "list_enums",
        .description = "Walk the live UEnum registry. Substring filter, "
                       "max_results 1..5000.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",      {{"type", "string"}}},
                {"max_results", {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "find_implementers",
        .description = "Classes that implement the given UInterface. Distinct "
                       "from class_hierarchy (which walks INHERITS_FROM); "
                       "interfaces are a separate axis. Returns {name, path, "
                       "module, is_native}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"interface_path", {{"type", "string"}}},
                {"max_results",    {{"type", "integer"}, {"minimum",1}, {"maximum",5000}}},
            }},
            {"required", nlohmann::json::array({"interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    registerRemote(sage::mcp::Tool{
        .name        = "class_default_object",
        .description = "Read the class default object's UProperty values as JSON. "
                       "Skips Transient/Deprecated. Read-only; write via Phase 4.2 "
                       "bp.set_cdo_property.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"class_path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"class_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Blueprint authoring (Phase 4.2 — read + write) ---------------
    auto bpPathSchema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"path"})},
        {"additionalProperties", false},
    };
    auto bpPathFnSchema = nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"path",     {{"type", "string"}}},
            {"function", {{"type", "string"}}},
        }},
        {"required", nlohmann::json::array({"path", "function"})},
        {"additionalProperties", false},
    };

    registerRemote(sage::mcp::Tool{
        .name = "bp.read",
        .description = "Blueprint summary: name, parent class, generated class, "
                       "variable/function/event-graph counts, implemented interfaces.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_variables",
        .description = "All UCLASS variables: name, type, default, category, flags "
                       "(EditAnywhere, Replicated, RepNotify, Transient, ...).",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_functions",
        .description = "All BP-side function/event/macro graphs with kind + node count.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_function_graph",
        .description = "All nodes + pins + connections for one graph (function or "
                       "event_graph). Each pin reports direction, type, default, "
                       "and link list with target node id + pin name.",
        .inputSchema = bpPathFnSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_execution_flow",
        .description = "BFS exec-pin walk from FunctionEntry / Event nodes — easier "
                       "for the agent than parsing the full graph. Returns ordered "
                       "[{order, id, class, title}].",
        .inputSchema = bpPathFnSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_components",
        .description = "SCS hierarchy: name, class, parent, attach socket per node. "
                       "Recursive walk from root nodes.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.search_nodes",
        .description = "Substring search across all of a Blueprint's graphs "
                       "(function + event + macro). Returns hits with graph, "
                       "graph_kind, id, title, class.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"keyword", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "keyword"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — variables --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_variable",
        .description = "Add a member variable. type=PinCategory string ('bool', "
                       "'int', 'float', 'string', 'object', 'struct', ...). "
                       "type_object resolves a UClass/UStruct path for object/"
                       "struct types. is_array=true for TArray. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"name",          {{"type", "string"}}},
                {"type",          {{"type", "string"}}},
                {"type_object",   {{"type", "string"}}},
                {"is_array",      {{"type", "boolean"}}},
                {"default_value", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name", "type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_variable",
        .description = "Remove a member variable + fix up references. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_variable_default",
        .description = "Set a member variable's default value (string-coerced). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"name",  {{"type", "string"}}},
                {"value", {{"description", "JSON value (string/number/bool)"}}},
            }},
            {"required", nlohmann::json::array({"path", "name", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — local variables (Phase 4.2 round 2b) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_local_variables",
        .description = "Enumerate function-scope (local) variables on the "
                       "function's UK2Node_FunctionEntry. Returns "
                       "{variables: [{name, type, type_object?, is_array?, "
                       "is_map?, is_set?, category, default_value?, flags}], "
                       "count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_local_variable",
        .description = "Add a function-scope variable via "
                       "FBlueprintEditorUtils::AddLocalVariable. Same type "
                       "shape as bp.add_variable: type (PinCategory: bool/"
                       "int/float/double/string/name/object/struct/...), "
                       "type_object? (sub-category UClass/UStruct/UEnum), "
                       "is_array?, default_value?. -32602 on duplicate name. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"function",      {{"type", "string"}}},
                {"name",          {{"type", "string"}}},
                {"type",          {{"type", "string"}}},
                {"type_object",   {{"type", "string"}}},
                {"is_array",      {{"type", "boolean"}}},
                {"default_value", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "name", "type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_local_variable",
        .description = "Remove a function-scope variable from the "
                       "UK2Node_FunctionEntry::LocalVariables array. Returns "
                       "{removed: int} (0 = name not found, no error). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"name",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — interfaces (Phase 4.2 round 2c) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_interfaces",
        .description = "Enumerate UInterface classes implemented by the BP. "
                       "Returns {interfaces: [{name, path, graph_count}], "
                       "count}. Reads UBlueprint::ImplementedInterfaces.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_interface",
        .description = "Implement a UInterface on the BP via "
                       "FBlueprintEditorUtils::ImplementNewInterface. "
                       "interface_path: '/Script/Foo.UMyInterface' or BP "
                       "interface asset path. Idempotent — already-implemented "
                       "returns {already: true}. Class must derive from "
                       "UInterface or -32602. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",           {{"type", "string"}}},
                {"interface_path", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_interface",
        .description = "Remove an implemented UInterface from the BP via "
                       "FBlueprintEditorUtils::RemoveInterface. "
                       "preserve_functions=true keeps the interface's "
                       "function graphs as regular BP functions (default "
                       "false: drops them). Returns {removed: 0|1} — 0 when "
                       "the interface wasn't implemented (idempotent). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",               {{"type", "string"}}},
                {"interface_path",     {{"type", "string"}}},
                {"preserve_functions", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path", "interface_path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — variable props + CDO + deps (Phase 4.2 round 2g/p5) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_variable_properties",
        .description = "Toggle BP member variable property flags + metadata. "
                       "All optional bools layer onto FBPVariableDescription"
                       "::PropertyFlags: instance_editable (CPF_Edit), "
                       "blueprint_readonly (CPF_BlueprintReadOnly), "
                       "replicated (CPF_Net; clears RepNotify on false), "
                       "transient (CPF_Transient), save_game (CPF_SaveGame), "
                       "expose_on_spawn (CPF_ExposeOnSpawn + MD_ExposeOnSpawn "
                       "metadata). Strings: category (MD_FunctionCategory), "
                       "tooltip (MD_Tooltip). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",                {{"type", "string"}}},
                {"name",                {{"type", "string"}}},
                {"instance_editable",   {{"type", "boolean"}}},
                {"blueprint_readonly",  {{"type", "boolean"}}},
                {"replicated",          {{"type", "boolean"}}},
                {"transient",           {{"type", "boolean"}}},
                {"save_game",           {{"type", "boolean"}}},
                {"expose_on_spawn",     {{"type", "boolean"}}},
                {"category",            {{"type", "string"}}},
                {"tooltip",             {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_cdo_properties",
        .description = "Read the Class Default Object of any UClass — "
                       "engine native (e.g. /Script/Engine.Actor) or BP-"
                       "generated. Optional 'properties' array filters to "
                       "specific names. Skips transient. Returns "
                       "{class, class_name, properties: {...}, count}. "
                       "Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class",      {{"type", "string"}}},
                {"properties", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_dependencies",
        .description = "Forward (default) or reverse (reverse=true) asset "
                       "dependencies via AssetRegistry. Forward also "
                       "enumerates class refs (parent + variable subtypes). "
                       "Returns {dependencies[], dependency_count, "
                       "referenced_classes[]} or {referencers[], "
                       "referencer_count} depending on direction.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}}},
                {"reverse", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — diagnostics + dry-run (Phase 4.2 round 2g/p4) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.validate",
        .description = "Compile the BP via FKismetEditorUtilities::"
                       "CompileBlueprint with SkipSave + a silent "
                       "FCompilerResultsLog and return the diagnostics. "
                       "Useful before bp.compile to surface errors / "
                       "warnings without dirtying the package. Returns "
                       "{valid, error_count, warning_count, messages: "
                       "[{severity, message}]} — severity ∈ {error, "
                       "warning, perf, info}. PIE rejected (compile is "
                       "still a write operation under the hood).",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.run_construction_script",
        .description = "Spawn a transient instance of the BP into the "
                       "editor world, let SpawnActor + RerunConstruction"
                       "Scripts run, snapshot the resulting components + "
                       "transforms, then destroy. Useful to inspect what "
                       "the construction script produces without leaving "
                       "an actor in the level. location {x,y,z} optional "
                       "(default origin). BP must be Actor-derived. "
                       "Returns {class, components: [{name, class, "
                       "location, rotation, scale, is_root}], count}. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"location", {{"type", "array"},
                              {"items", {{"type", "number"}}},
                              {"minItems", 3}, {"maxItems", 3}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — SCS component deep CRUD (Phase 4.2 round 2g/p3) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_component_properties",
        .description = "Dump every reflected UProperty on the BP's SCS "
                       "component template (USCS_Node->ComponentTemplate). "
                       "Skips transient / DuplicateTransient. Uses Sage's "
                       "GetUPropertyAsJson — primitives, structs (Vector/"
                       "Rotator/Transform/Color/...), object refs, "
                       "TArray/TMap/TSet, enums all round-tripped. Returns "
                       "{class, properties: {...}, count}. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"component", {{"type", "string"},
                               {"description", "SCS variable name (e.g. 'Mesh', 'Capsule')"}}},
            }},
            {"required", nlohmann::json::array({"path", "component"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.get_component_property",
        .description = "Read a single UProperty on a SCS component "
                       "template. Cheaper than bp.read_component_properties "
                       "when only one value is needed. Returns "
                       "{type (FProperty class name), value}. -32602 if "
                       "the property doesn't exist on the component class.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"component", {{"type", "string"}}},
                {"property",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "component", "property"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.reparent_component",
        .description = "Move a SCS component under a different parent in "
                       "the BP's component hierarchy (USCS_Node tree). "
                       "Cycle-guarded — rejects with -32602 if the new "
                       "parent is a descendant of the moved component. "
                       "Detaches from the current parent (or root list) "
                       "and attaches to the new parent. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",       {{"type", "string"}}},
                {"component",  {{"type", "string"}}},
                {"new_parent", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "component", "new_parent"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — T3D node clipboard (Phase 4.2 round 2g/p2) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.export_nodes_t3d",
        .description = "Export BP graph nodes to UE's T3D ASCII format via "
                       "FEdGraphUtilities::ExportNodesToText. Mirrors the "
                       "editor's Copy operation. node_ids[] selects specific "
                       "nodes by FGuid; omit/empty exports the whole graph. "
                       "Entry/return nodes are skipped (CanDuplicateNode "
                       "filter). Returns {t3d, count, skipped}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.import_nodes_t3d",
        .description = "Import T3D-formatted nodes into a BP graph via "
                       "FEdGraphUtilities::ImportNodesFromText. Pre-checks "
                       "with CanImportNodesFromText (-32602 on schema "
                       "mismatch / malformed text). Pasted nodes get fresh "
                       "FGuids so re-pasting into the same graph doesn't "
                       "collide. Optional pos_x + pos_y anchors the pasted "
                       "set's centroid at the given position. Returns "
                       "{count, node_ids[], recentered}. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"t3d",      {{"type", "string"}}},
                {"pos_x",    {{"type", "number"}}},
                {"pos_y",    {{"type", "number"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "t3d"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — event dispatchers (Phase 4.2 round 2g/p1) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_event_dispatchers",
        .description = "Enumerate event dispatchers (multicast delegates) "
                       "on the BP. Reads UBlueprint::DelegateSignatureGraphs. "
                       "Each entry: {name, graph, node_count, parameters: "
                       "[{name, type, direction:input}]}. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_event_dispatcher",
        .description = "Create a new event dispatcher. Wires up both halves: "
                       "the '<Name>__DelegateSignature' graph in "
                       "DelegateSignatureGraphs (UEdGraphSchema_K2 default "
                       "nodes) AND a member variable of type PC_MCDelegate "
                       "referencing the signature graph. Use "
                       "bp.add_function_parameter on the signature graph to "
                       "configure dispatcher payload types. -32602 on "
                       "duplicate name. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_event_dispatcher",
        .description = "Remove a dispatcher: deletes the signature graph "
                       "AND the member variable. Returns {removed: 0|1} "
                       "(idempotent — 0 when name absent on both sides). "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — asset creation (Phase 4.2 round 2f) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.create",
        .description = "Create a new Blueprint asset via UBlueprintFactory + "
                       "IAssetTools::CreateAsset. parent_class accepts a "
                       "full path ('/Script/Engine.Actor') or a short name "
                       "('Actor', 'Pawn', 'Character'); defaults to Actor. "
                       "path: '/Game/Folder/BP_Foo' or '/Game/Folder/BP_Foo."
                       "BP_Foo'. Idempotent — returns {already: true} if "
                       "the asset already exists. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",         {{"type", "string"}}},
                {"parent_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.create_interface",
        .description = "Create a new Blueprint Interface asset via "
                       "UBlueprintInterfaceFactory. ParentClass is "
                       "UInterface (set by the factory). path same form "
                       "as bp.create. Idempotent. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — function parameter I/O (Phase 4.2 round 2e) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_function_parameters",
        .description = "Enumerate user-defined input/output parameters on "
                       "a BP function. Inputs come from "
                       "UK2Node_FunctionEntry::UserDefinedPins, outputs "
                       "from UK2Node_FunctionResult::UserDefinedPins (may "
                       "be empty if no FunctionResult node yet). Returns "
                       "{inputs: [{name, type, direction, type_object?, "
                       "is_array?, default_value?}], outputs: [...], "
                       "input_count, output_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_function_parameter",
        .description = "Add a parameter (input or output) to a BP function "
                       "via UK2Node_EditablePinBase::CreateUserDefinedPin. "
                       "direction='input' (default) targets FunctionEntry, "
                       "'output' targets FunctionResult (auto-spawned if "
                       "the function has no result node yet). Same type "
                       "shape as bp.add_variable. -32602 on duplicate name. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"function",    {{"type", "string"}}},
                {"name",        {{"type", "string"}}},
                {"type",        {{"type", "string"}}},
                {"direction",   {{"type", "string"},
                                 {"enum", nlohmann::json::array({"input","output"})}}},
                {"type_object", {{"type", "string"}}},
                {"is_array",    {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"path","function","name","type"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.remove_function_parameter",
        .description = "Remove a parameter from a BP function via "
                       "RemoveUserDefinedPinByName on the appropriate "
                       "Entry / Result node. direction='input' (default) "
                       "or 'output'. Returns {removed: 0|1} (idempotent — "
                       "0 when name absent or no result node). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"function",  {{"type", "string"}}},
                {"name",      {{"type", "string"}}},
                {"direction", {{"type", "string"},
                               {"enum", nlohmann::json::array({"input","output"})}}},
            }},
            {"required", nlohmann::json::array({"path","function","name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- read+write — graph management (Phase 4.2 round 2d) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_graphs",
        .description = "Enumerate all UEdGraphs on the BP. Returns "
                       "{graphs: [{name, kind, node_count}], count} where "
                       "kind ∈ {ubergraph, function, delegate, macro}. "
                       "Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.rename_function",
        .description = "Rename a user function graph via "
                       "FBlueprintEditorUtils::RenameGraph. Reflects on the "
                       "compiled UFunction at next compile. -32602 if "
                       "old_name not found, new_name already exists, or "
                       "old_name == new_name. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"old_name", {{"type", "string"}}},
                {"new_name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "old_name", "new_name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — functions --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_function",
        .description = "Create a new user function graph. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_function",
        .description = "Remove a user function graph. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "name"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — graph nodes --
    registerRemote(sage::mcp::Tool{
        .name = "bp.delete_node",
        .description = "Remove a node from a graph by GUID. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "function", "node_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.connect_pins",
        .description = "Connect two pins. Schema-validated; rejects type-incompatible "
                       "links. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"function",  {{"type", "string"}}},
                {"from_node", {{"type", "string"}}},
                {"from_pin",  {{"type", "string"}}},
                {"to_node",   {{"type", "string"}}},
                {"to_pin",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","function","from_node","from_pin","to_node","to_pin"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — graph node CRUD (Phase 4.2 round 2a) --
    registerRemote(sage::mcp::Tool{
        .name = "bp.add_node",
        .description = "Spawn a UEdGraphNode subclass into the target graph. "
                       "node_class accepts a full class name (K2Node_*) or one of "
                       "the aliases: CallFunction, Event, CustomEvent, GetVar, "
                       "SetVar, Branch (=K2Node_IfThenElse), If. node_params for "
                       "CallFunction: {function_name, target_class}. node_params "
                       "for GetVar/SetVar: {variable_name}. Returns "
                       "{node_id (guid), node_class, pos, pins[]}. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"function",    {{"type", "string"}}},
                {"node_class",  {{"type", "string"}}},
                {"node_x",      {{"type", "number"}}},
                {"node_y",      {{"type", "number"}}},
                {"node_params", {{"type", "object"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_node_property",
        .description = "Set a pin's default value. Goes through the schema's "
                       "TrySetDefaultValue (type-coerces and validates). "
                       "Execution pins are rejected — use bp.connect_pins. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
                {"pin",      {{"type", "string"}}},
                {"value",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_id","pin","value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.read_node_property",
        .description = "Read a pin's default value + type metadata. Returns "
                       "{type, direction, default_value, default_object?, "
                       "default_text?, is_execution, link_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"function", {{"type", "string"}}},
                {"node_id",  {{"type", "string"}}},
                {"pin",      {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","function","node_id","pin"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.list_node_types",
        .description = "Enumerate concrete UK2Node subclasses available to the "
                       "BP palette. filter (substring match on class name), max "
                       "(default 200, clamped 1..2000). Returns {types: "
                       "[{name, module}], returned, total}. Excludes Abstract / "
                       "Deprecated / NewerVersionExists classes.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter", {{"type", "string"}}},
                {"max",    {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- write — class shape --
    registerRemote(sage::mcp::Tool{
        .name = "bp.set_cdo_property",
        .description = "Write a property on the BP's class default object. Goes "
                       "through Phase 4.0 reflection (TArray/TMap/TObjectPtr/"
                       "FStruct/UEnum all supported). Marks BP structurally "
                       "modified. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}}},
                {"property", {{"type", "string"}}},
                {"value",    {{"description", "JSON value matching property type"}}},
            }},
            {"required", nlohmann::json::array({"path", "property", "value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "bp.reparent",
        .description = "Change BP's parent class. Refreshes all nodes for the new "
                       "parent's interface. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",             {{"type", "string"}}},
                {"new_parent_class", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "new_parent_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // -- compile --
    registerRemote(sage::mcp::Tool{
        .name = "bp.compile",
        .description = "Trigger full BP compile. Returns {success, errors, "
                       "warnings, notes}. PIE rejected.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });

    // ---- Material graph (Phase 4.3) ------------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "mat.read",
        .description = "Material/instance summary: domain, blend_mode, "
                       "shading_model, two_sided, expression count, base "
                       "material name.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.list_parameters",
        .description = "Scalar/vector/texture/static-switch parameters with name + kind.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.list_expressions",
        .description = "All graph nodes (UMaterialExpression*) with id, class, "
                       "x/y position, and shorthand value when applicable "
                       "(scalar/vector/parameter constants).",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.create_instance",
        .description = "Create a UMaterialInstanceConstant from a parent material. "
                       "destination is the new asset's path "
                       "(e.g. /Game/Mats/MI_Foo). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"parent",      {{"type", "string"}}},
                {"destination", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"parent", "destination"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.add_expression",
        .description = "Add a UMaterialExpression node. expression_class accepts "
                       "engine-path form (/Script/Engine.MaterialExpressionConstant). "
                       "Returns the new node's expression_id (GUID). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",             {{"type", "string"}}},
                {"expression_class", {{"type", "string"}}},
                {"x",                {{"type", "integer"}}},
                {"y",                {{"type", "integer"}}},
            }},
            {"required", nlohmann::json::array({"path", "expression_class"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.delete_expression",
        .description = "Remove an expression by GUID. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path", "expression_id"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_expressions",
        .description = "Wire two expression outputs/inputs. from_output / to_input "
                       "are pin name strings. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"from_id",     {{"type", "string"}}},
                {"to_id",       {{"type", "string"}}},
                {"from_output", {{"type", "string"}}},
                {"to_input",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","from_id","to_id","from_output","to_input"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_to_property",
        .description = "Wire an expression output into a material property. "
                       "property: BaseColor, Metallic, Roughness, Specular, "
                       "EmissiveColor, Normal, Opacity, OpacityMask, "
                       "WorldPositionOffset. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}}},
                {"from_id",     {{"type", "string"}}},
                {"from_output", {{"type", "string"}}},
                {"property",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","from_id","from_output","property"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_expression_value",
        .description = "Edit a constant in the graph. Constant→number, "
                       "Constant3Vector→[r,g,b]/[r,g,b,a], ScalarParameter→"
                       "default value. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
                {"value",         {{"description", "number / [r,g,b] / [r,g,b,a]"}}},
            }},
            {"required", nlohmann::json::array({"path","expression_id","value"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.connect_texture",
        .description = "Set the UTexture on a TextureBase expression "
                       "(e.g. TextureSample). PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",          {{"type", "string"}}},
                {"expression_id", {{"type", "string"}}},
                {"texture",       {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array(
                {"path","expression_id","texture"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_shading_model",
        .description = "Switch the base material's shading model (Unlit, "
                       "DefaultLit, Subsurface, ClearCoat, Hair, Cloth, Eye, "
                       "ThinTranslucent, ...). Recompiles. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"model", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"path","model"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.set_base_color",
        .description = "Adds a Constant3Vector at -300/0 and wires it into BaseColor. "
                       "color: [r,g,b] or [r,g,b,a] (linear, 0..1). Recompiles. "
                       "PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",  {{"type", "string"}}},
                {"color", {{"type", "array"}, {"items", {{"type", "number"}}},
                           {"minItems", 3}, {"maxItems", 4}}},
            }},
            {"required", nlohmann::json::array({"path","color"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "mat.validate",
        .description = "Recompile the material (or refresh the instance) and "
                       "report success.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });

    // ---- Asset advanced (Phase 4.5) ------------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_mesh_bounds",
        .description = "Static or skeletal mesh bounding box + extent + sphere "
                       "radius + local-space min/max. Read-only.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.get_mesh_collision",
        .description = "Mesh collision primitive counts (box/sphere/capsule/"
                       "convex) + total + collision complexity flag. Read-only.",
        .inputSchema = bpPathSchema, .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.list_redirectors",
        .description = "List UObjectRedirector assets under a folder (default "
                       "/Game). Returned for cleanup planning before "
                       "fixup_redirectors.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"folder", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.diagnose_registry",
        .description = "AssetRegistry health snapshot: total, in_memory, "
                       "on_disk_only, transient. Useful when the agent suspects "
                       "stale registry state.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.bulk_rename",
        .description = "Atomic multi-rename in a single transaction. "
                       "renames=[{src, dst}]. Each entry uses the same "
                       "UEditorAssetSubsystem::RenameAsset; failures are "
                       "reported per-row with ok:false. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"renames", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"src", {{"type", "string"}}},
                            {"dst", {{"type", "string"}}},
                        }},
                        {"required", nlohmann::json::array({"src","dst"})},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"renames"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.move_folder",
        .description = "Move every asset under src into dst, preserving "
                       "subfolder structure. UE leaves redirectors at the "
                       "old paths automatically — pair with "
                       "asset.fixup_redirectors to clean up. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"src", {{"type", "string"}}},
                {"dst", {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"src","dst"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "asset.fixup_redirectors",
        .description = "Resolve referencers and remove redirectors under the "
                       "given folders (default /Game). Uses IAssetTools::"
                       "FixupReferencers — referencing assets get re-saved "
                       "to point at the new path, then the redirector is "
                       "deleted. PIE rejected.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"paths", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection (Phase 4.7 batch 1) -----------------------
    registerRemote(sage::mcp::Tool{
        .name = "project.get_info",
        .description = "Read .uproject metadata: project_name, project_dir, "
                       "engine_dir, engine_association, description, "
                       "category, declared_modules[], plugins[]. Read-only.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.list_modules",
        .description = "Walk Source/<Module>/<Module>.Build.cs and report "
                       "each native module: {name, module_dir, "
                       "build_cs_path, header_count, source_count}. "
                       "Lightweight discovery before drilling into a "
                       "specific module via read_cpp_header.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_cpp_header",
        .description = "Read a .h file and regex-scan for UCLASS / "
                       "USTRUCT / UENUM declarations + #include directives. "
                       "path is relative to project root or absolute "
                       "(must be under ProjectDir or EngineDir for "
                       "safety). Returns {classes[], structs[], enums[], "
                       "includes[], line_count}. Each declaration is "
                       "{name, line}. NOT a full UHT parse — heuristic "
                       "regex; gets ~95% of common UE headers right.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_cpp_source",
        .description = "Read a .h / .cpp / .inl source file as a string, "
                       "capped at max_bytes (default 64KB, clamped "
                       "1KB..512KB). Path same form as read_cpp_header. "
                       "Returns {content, size_bytes, truncated, "
                       "max_bytes}. truncated=true means orig file was "
                       "larger and content has been Left()'d.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path",      {{"type", "string"}}},
                {"max_bytes", {{"type", "integer"},
                               {"minimum", 1024}, {"maximum", 524288}}},
            }},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Project introspection batch 2 (Phase 4.7 batch 2) -------------
    registerRemote(sage::mcp::Tool{
        .name = "project.search_cpp",
        .description = "Substring search across the project's Source/ "
                       "subtree (.h, .cpp, .inl). Case-sensitive. Returns "
                       "{hits: [{file, line, snippet}], count, capped}. "
                       "max_results clamped 1..500 (default 50). Snippet "
                       "trimmed/capped at 200 chars.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query",       {{"type", "string"}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
            }},
            {"required", nlohmann::json::array({"query"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.list_engine_modules",
        .description = "Enumerate engine native modules under Engine/Source/"
                       "{Runtime,Editor,Developer,ThirdParty}. Returns "
                       "{name, category, module_dir} per module.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.read_engine_header",
        .description = "Same shape as project.read_cpp_header but the path "
                       "must live under EngineDir (the safety guard rejects "
                       "anywhere else). Convenience alias so the agent's "
                       "intent is clear.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({"path"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "project.find_engine_symbol",
        .description = "Substring search across an engine source category "
                       "tree (.h + .cpp). category ∈ {Runtime (default), "
                       "Editor, Developer, ThirdParty}. max_results clamped "
                       "1..500 (default 50). Returns same shape as "
                       "search_cpp plus the category and root.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"symbol",      {{"type", "string"}}},
                {"category",    {{"type", "string"},
                                 {"enum", nlohmann::json::array({
                                     "Runtime","Editor","Developer","ThirdParty"})}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
            }},
            {"required", nlohmann::json::array({"symbol"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Editor automation (Phase 4.6) ---------------------------------
    registerRemote(sage::mcp::Tool{
        .name = "editor.console_command",
        .description = "Execute a UE console command. Default: gated to a "
                       "read-only / view-state whitelist (STAT, SHOW, "
                       "VIEWMODE, CAMERA, R.SCREENPERCENTAGE, MEMREPORT, "
                       "OBJ, LOG, HELP). allow_unsafe=true bypasses — UE "
                       "console can crash the editor with the wrong command, "
                       "use with care.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"cmd",          {{"type", "string"}}},
                {"allow_unsafe", {{"type", "boolean"}}},
            }},
            {"required", nlohmann::json::array({"cmd"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.take_screenshot",
        .description = "Capture the active viewport as a PNG. Default path: "
                       "ProjectSavedDir/Screenshots/Sage_<timestamp>.png. "
                       "Returns the saved path. Async — UE writes the file "
                       "shortly after the call returns.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_engine_version",
        .description = "Engine + project + build configuration metadata: "
                       "engine_version, compatible_version, "
                       "build_configuration, project_dir, engine_dir.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_project_version",
        .description = "Project name + dir + log/saved/content paths.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_log_file_path",
        .description = "Absolute path of the current editor log file. The "
                       "agent can tail it directly via filesystem when more "
                       "than read_log's slice is needed.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.read_log",
        .description = "Tail recent editor log lines. filter (substring) and "
                       "max_lines (1..5000, default 200). Returns {log_path, "
                       "lines[], count, total_lines}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"filter",    {{"type", "string"}}},
                {"max_lines", {{"type", "integer"},
                               {"minimum", 1}, {"maximum", 5000}}},
            }},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });

    // ---- Dialog policy (Phase 4.6 round 2) -----------------------------
    // Hooks FCoreDelegates::ModalMessageDialog so unattended agent flows
    // don't stall on Save?/Reload?/Confirm Delete? modals. Lazy install
    // on first set_dialog_policy call; hook stays bound for the editor
    // session lifetime and is unbound on plugin module shutdown.
    registerRemote(sage::mcp::Tool{
        .name = "editor.set_dialog_policy",
        .description = "Auto-respond to any modal whose title or message "
                       "contains 'pattern' (substring). response ∈ "
                       "{yes, no, ok, cancel, retry, continue, yesall, "
                       "noall}. Replaces an existing policy with the same "
                       "pattern. First call lazy-installs the dialog hook.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"pattern",  {{"type", "string"}}},
                {"response", {{"type", "string"},
                              {"enum", nlohmann::json::array({
                                  "yes", "no", "ok", "cancel", "retry",
                                  "continue", "yesall", "noall"})}}},
            }},
            {"required", nlohmann::json::array({"pattern", "response"})},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.clear_dialog_policy",
        .description = "Remove a policy by exact-pattern match, or all "
                       "policies when 'pattern' is omitted. Returns "
                       "{removed, policy_count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {{"pattern", {{"type", "string"}}}}},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.get_dialog_policy",
        .description = "List active dialog policies and the hook install "
                       "status. Returns {policies[], count, hook_installed}.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.list_dialogs",
        .description = "Walk Slate to describe the currently-active modal "
                       "(UE shows at most one). Returns {dialogs: [{title, "
                       "message, buttons[]}], count}.",
        .inputSchema = nlohmann::json{
            {"type", "object"}, {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        },
        .handler = nullptr, .remote = true,
    });
    registerRemote(sage::mcp::Tool{
        .name = "editor.respond_to_dialog",
        .description = "Click a button on the active modal. Provide "
                       "button_index (0-based), button_label (substring), "
                       "or action='escape' to dismiss. -32004 if no modal "
                       "is active; -32602 with available_buttons[] if no "
                       "match.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"button_index", {{"type", "integer"}, {"minimum", 0}}},
                {"button_label", {{"type", "string"}}},
                {"action",       {{"type", "string"},
                                  {"enum", nlohmann::json::array({"escape"})}}},
            }},
            {"additionalProperties", false},
        },
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

    sage::mcp::Tool classHierarchyTool{
        .name        = "class_hierarchy",
        .description = "Walk INHERITS_FROM edges from a UClass. "
                       "direction='ancestors' returns parent chain (Pawn → "
                       "Actor → Object); 'descendants' returns subclasses "
                       "(Pawn → all pawn types). max_depth 1..10 (default 10 "
                       "covers UE's typical inheritance depth). Returns "
                       "[{name, module, is_native, depth}] ordered by depth. "
                       "Class table is populated by index_slot from UE's "
                       "reflected UClass registry.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"class_name",  {{"type", "string"}}},
                {"direction",   {{"type", "string"},
                                 {"enum", nlohmann::json::array({"ancestors","descendants"})}}},
                {"max_depth",   {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 10}}},
                {"max_results", {{"type", "integer"},
                                 {"minimum", 1}, {"maximum", 500}}},
                {"slot_id",     {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"class_name"})},
            {"additionalProperties", false},
        },
        .handler = [graphMgr, resolveSlotId, escCypher](const nlohmann::json& params)
            -> sage::mcp::ToolResult {
            if (!params.is_object() || !params.contains("class_name")
                || !params["class_name"].is_string()) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams, "missing 'class_name'"));
            }
            const auto cls = params["class_name"].get<std::string>();
            const auto dir = params.value("direction", std::string{"ancestors"});
            if (dir != "ancestors" && dir != "descendants") {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InvalidParams,
                    "direction must be 'ancestors' or 'descendants'"));
            }
            const int  maxDepth   = std::clamp(params.value("max_depth",   10), 1, 10);
            const int  maxResults = std::clamp(params.value("max_results", 200), 1, 500);

            auto slot = resolveSlotId(params);
            if (!slot.has_value()) return std::unexpected(slot.error());

            try {
                auto& store = graphMgr->acquireSlot(*slot);
                std::ostringstream q;
                if (dir == "ancestors") {
                    q << "MATCH (c:Class {name: " << escCypher(cls) << "})"
                      << "-[r:INHERITS_FROM*1.." << maxDepth << "]->(a:Class) ";
                } else {
                    q << "MATCH (c:Class {name: " << escCypher(cls) << "})"
                      << "<-[r:INHERITS_FROM*1.." << maxDepth << "]-(a:Class) ";
                }
                q << "RETURN DISTINCT a.name AS name, a.module AS module, "
                  << "       a.is_native AS is_native "
                  << "ORDER BY name LIMIT " << maxResults << ";";

                auto r = store.execute(q.str());
                if (sage::graph::is_error(r)) {
                    return std::unexpected(sage::mcp::ErrorObject::fromCode(
                        sage::mcp::ErrorCode::InternalError,
                        sage::graph::error_of(r).message));
                }
                const auto& env = sage::graph::value_of(r);
                nlohmann::json out = nlohmann::json::object();
                out["class_name"] = cls;
                out["direction"]  = dir;
                out["max_depth"]  = maxDepth;
                out["classes"]    = env["rows"];
                out["count"]      = env["row_count"];
                out["truncated"]  = env["row_count"].get<int64_t>() == maxResults;
                out["slot_id"]    = *slot;
                return out;
            } catch (const std::exception& ex) {
                return std::unexpected(sage::mcp::ErrorObject::fromCode(
                    sage::mcp::ErrorCode::InternalError,
                    std::string{"class_hierarchy failed: "} + ex.what()));
            }
        },
        .remote = false,
    };
    if (auto r = registry->registerTool(std::move(classHierarchyTool)); !r.has_value()) {
        spdlog::warn("Failed to register 'class_hierarchy'");
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
