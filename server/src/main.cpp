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
#include "mcp/server.h"
#include "mcp/tool_registry.h"
#include "tools/builtin.h"
#include "transport/http_sse_server.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
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
