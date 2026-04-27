#pragma once

#include "mcp/tool_registry.h"
#include "mcp/types.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>

namespace sage::mcp {

struct ServerInfo {
    std::string name{"sage-unreal-mcp"};
    std::string version{"0.1.0"};
};

// Dispatches JSON-RPC envelopes to either built-in MCP methods (initialize,
// ping, tools/list, tools/call) or registered tools.
//
// Concurrency: handle() is reentrant when registry is read-only post-init.
// Connection-level threading is the transport's responsibility.
class MCPServer {
public:
    MCPServer(ServerInfo info, std::shared_ptr<ToolRegistry> registry);

    [[nodiscard]] std::optional<Response> handle(const Request& request);

    // Convenience: parse + handle a raw JSON envelope. Returns nullopt for
    // notifications. On parse failure, surfaces a JSON-RPC InvalidRequest
    // response with null id.
    [[nodiscard]] std::optional<nlohmann::json> handleRaw(const nlohmann::json& payload);

    [[nodiscard]] const ToolRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] const ServerInfo& info() const noexcept { return info_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    [[nodiscard]] Response onInitialize(Id id, const nlohmann::json& params);
    [[nodiscard]] Response onPing(Id id);
    [[nodiscard]] Response onToolsList(Id id);
    [[nodiscard]] Response onToolsCall(Id id, const nlohmann::json& params);

    ServerInfo info_;
    std::shared_ptr<ToolRegistry> registry_;
    bool initialized_{false};
};

}  // namespace sage::mcp
