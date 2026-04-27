#pragma once

#include "mcp/error_codes.h"
#include "mcp/types.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <functional>
#include <string>

namespace sage::mcp {

// std::expected channel for tool outcomes. ErrorObject carries Sage-specific
// codes (api-spec.md §Error Codes) so transports can propagate them faithfully.
using ToolResult = std::expected<nlohmann::json, ErrorObject>;

// Synchronous handler signature. Streaming/async variant (SSE-driven) will be
// introduced once a tool actually needs progressive output (e.g. compile,
// indexing). Phase 1 only ships `ping`.
using ToolHandler = std::function<ToolResult(const nlohmann::json& params)>;

struct Tool {
    std::string name;            // canonical identifier (e.g. "ping", "spawn_actor")
    std::string description;     // human-readable; agents use this to reason about applicability
    nlohmann::json inputSchema;  // JSON Schema describing params (api-spec.md §Token Optimization)
    ToolHandler handler;
};

}  // namespace sage::mcp
