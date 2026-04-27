// Sage server entry point.
//
// Wires:
//   ToolRegistry  ← registerBuiltins (ping)
//   MCPServer     ← registry
//   HTTP+SSE      ← MCPServer
// Listens until SIGINT / SIGTERM, then graceful shutdown.

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
#include <stdexcept>
#include <string>

namespace {

std::atomic<sage::transport::HttpSseServer*> g_runningServer{nullptr};

extern "C" void signalHandler(int signal) {
    // Async-signal-safe: only touch atomics & call into httplib::Server::stop()
    // which is documented as safe from a signal handler context.
    if (auto* s = g_runningServer.load(std::memory_order_acquire)) {
        s->stop();
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

    sage::transport::HttpSseConfig cfg{
        .host            = envOr("SAGE_HTTP_HOST", "127.0.0.1"),
        .port            = envIntOr("SAGE_HTTP_PORT", 7777),
        .mcpEndpoint     = "/mcp",
        .readTimeoutSec  = 30,
        .writeTimeoutSec = 30,
    };
    sage::transport::HttpSseServer transport(mcpServer, cfg);
    g_runningServer.store(&transport, std::memory_order_release);

    std::signal(SIGINT,  &signalHandler);
    std::signal(SIGTERM, &signalHandler);

    const bool ok = transport.listen();
    g_runningServer.store(nullptr, std::memory_order_release);

    if (!ok) {
        spdlog::error("HTTP server failed to bind {}:{}", cfg.host, cfg.port);
        return 1;
    }
    spdlog::info("Bye");
    return 0;
}
