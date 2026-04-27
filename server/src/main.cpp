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
