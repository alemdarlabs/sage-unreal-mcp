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
