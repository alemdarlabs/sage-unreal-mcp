#include "tools/builtin.h"

#include "mcp/tool.h"
#include "mcp/tool_registry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace sage::tools {

namespace {

mcp::ToolResult pingHandler(const nlohmann::json& params) {
    nlohmann::json result = nlohmann::json::object();
    if (params.is_object() && params.contains("echo")) {
        result["echo"] = params["echo"];
    }
    result["pong"] = true;
    return result;
}

void registerPing(mcp::ToolRegistry& registry) {
    mcp::Tool tool{
        .name        = "ping",
        .description = "Liveness check. Returns {pong: true} and echoes the optional "
                       "'echo' field. Use as a connection sanity test from the agent.",
        .inputSchema = nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"echo", {
                    {"type",        "string"},
                    {"description", "Optional string echoed back in the result"},
                }},
            }},
            {"additionalProperties", false},
        },
        .handler = pingHandler,
    };
    auto outcome = registry.registerTool(std::move(tool));
    if (!outcome.has_value()) {
        spdlog::error("Failed to register tool 'ping' — registry returned error");
    }
}

}  // namespace

void registerBuiltins(mcp::ToolRegistry& registry) {
    registerPing(registry);
}

}  // namespace sage::tools
