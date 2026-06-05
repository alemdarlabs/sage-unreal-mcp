#pragma once

#include "mcp/tool_registry.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace sage::tools {

struct SageGuidanceContext {
    std::string serverVersion;
    std::string protocolVersion;
    std::string transport;
    std::string httpEndpoint;
    std::string bridgeEndpoint;
    std::function<nlohmann::json()> editorSessions;
    std::function<nlohmann::json()> registrySummary;
};

// Register local, server-side guidance tools. These tools make Sage
// self-describing for MCP clients before an agent has read README/AGENTS.md.
void registerSageGuidanceTools(mcp::ToolRegistry& registry, SageGuidanceContext context);

}  // namespace sage::tools
