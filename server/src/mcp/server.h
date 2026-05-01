#pragma once

#include "mcp/tool_registry.h"
#include "mcp/types.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
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
    // Sink for server-pushed JSON-RPC notifications (id-less envelopes). The
    // transport (stdio, HTTP+SSE) installs this once at startup; the sink
    // MUST be thread-safe — publishNotification() is invoked from arbitrary
    // threads (bridge worker for editor connect/disconnect, etc.).
    using NotificationSink = std::function<void(const nlohmann::json&)>;

    MCPServer(ServerInfo info, std::shared_ptr<ToolRegistry> registry);

    [[nodiscard]] std::optional<Response> handle(const Request& request);

    // Convenience: parse + handle a raw JSON envelope. Returns nullopt for
    // notifications. On parse failure, surfaces a JSON-RPC InvalidRequest
    // response with null id.
    [[nodiscard]] std::optional<nlohmann::json> handleRaw(const nlohmann::json& payload);

    // Install the transport-side sink. No-op if not yet installed —
    // publishNotification() will silently drop until a sink exists.
    void setNotificationSink(NotificationSink sink);

    // Build a JSON-RPC 2.0 notification envelope and forward it to the sink.
    // Method should be a `notifications/<area>` form per MCP spec; for
    // sage-internal events the spec-canonical `notifications/message` is
    // used so MCP clients with default logging handlers will surface it.
    void publishNotification(std::string method, nlohmann::json params);

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

    mutable std::mutex sinkMu_;
    NotificationSink   sink_;
};

}  // namespace sage::mcp
