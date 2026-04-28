#pragma once

#include "bridge/editor_session.h"
#include "bridge/protocol.h"
#include "mcp/tool.h"  // mcp::ToolResult, mcp::ErrorObject

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ix {
class WebSocketServer;
class WebSocket;
class ConnectionState;
struct WebSocketMessage;
using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>;
}  // namespace ix

namespace sage::bridge {

struct BridgeConfig {
    std::string host = "127.0.0.1";
    int port = 7778;
    std::string endpoint = "/bridge";
    // Default deadline for sync tool dispatch when caller doesn't supply one.
    std::chrono::milliseconds defaultDispatchTimeout{30'000};
};

// WebSocket bridge between Sage server and N Unreal Editor plugins.
//
// Lifecycle:
//   1. Plugin connects (TCP + WS upgrade)
//   2. Plugin sends `hello` → server validates, registers session, replies `welcome`
//   3. Heartbeat exchange (15s default per ADR-001 / api-spec.md)
//   4. Tool dispatch: server→plugin `tool_call`, plugin→server `tool_result`
//   5. Close → server prunes session
//
// Thread model:
//   - ixwebsocket spins worker threads; callbacks (open/close/message) fire there.
//   - dispatchTool() is invoked from MCP request threads (cpp-httplib worker).
//   - Both share `pending_` (mutex) and `sessions_` (mutex).
//   - Promises bridge the two: dispatchTool() awaits future, callback resolves promise.
//
// Multi-editor routing (slot-aware) lands in Milestone 1.5; today dispatchTool()
// targets the first active session.
class BridgeServer {
public:
    explicit BridgeServer(BridgeConfig cfg);
    ~BridgeServer();

    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] std::vector<EditorSession> snapshotSessions() const;
    [[nodiscard]] std::optional<EditorSession> snapshotSession(std::string_view sessionId) const;

    // Resolves a label or session-id to a session. Returns the matching session
    // (label takes precedence if both ambiguous; nullopt otherwise).
    [[nodiscard]] std::optional<EditorSession> findByIdOrLabel(std::string_view idOrLabel) const;

    [[nodiscard]] std::size_t sessionCount() const;

    // Active session pointer (per-server, not per-MCP-client; ADR-004 §2 active
    // editor pointer arrives in full at Milestone 1.5b).
    void setActiveSession(std::string sessionId);
    [[nodiscard]] std::string activeSession() const;

    [[nodiscard]] const BridgeConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    // Synchronous tool dispatch: send `tool_call` to the first active session,
    // block until matching `tool_result` arrives or `timeout` fires. On no
    // active plugin, returns ErrorCode::EditorNotConnected.
    [[nodiscard]] mcp::ToolResult dispatchTool(std::string_view tool,
                                                const nlohmann::json& args,
                                                std::chrono::milliseconds timeout);

    // Convenience: uses BridgeConfig::defaultDispatchTimeout.
    [[nodiscard]] mcp::ToolResult dispatchTool(std::string_view tool,
                                                const nlohmann::json& args);

    [[nodiscard]] std::size_t pendingCount() const;

    // Async event handler: invoked off the bridge worker thread for every
    // `event` envelope received from any plugin. Caller (main.cpp) sets this
    // once at startup with the slot-aware graph patcher. `slot_id` is
    // resolved from the originating session's Hello.
    using EventHandler = std::function<void(std::string_view slot_id,
                                             const EventMessage& ev)>;
    void setEventHandler(EventHandler handler);

private:
    void onClientMessage(const std::shared_ptr<ix::ConnectionState>& state,
                         ix::WebSocket& ws,
                         const ix::WebSocketMessagePtr& msg);

    void handleHello(ix::WebSocket& ws, const std::string& session_id, const Json& payload);
    void handleHeartbeat(ix::WebSocket& ws, const std::string& session_id, const Json& payload);
    void handleToolResult(const std::string& session_id, const Json& payload);
    void handleEvent(const std::string& session_id, const Json& payload);

    [[nodiscard]] std::string nextTxId();

    struct PendingRpc {
        std::promise<mcp::ToolResult> promise;
    };

    BridgeConfig                                      cfg_;
    std::unique_ptr<ix::WebSocketServer>              server_;
    std::atomic<bool>                                 running_{false};
    std::atomic<std::uint64_t>                        txCounter_{0};

    mutable std::mutex                                sessionsMu_;
    std::unordered_map<std::string, EditorSession>    sessions_;  // keyed by ConnectionState id

    mutable std::mutex                                pendingMu_;
    std::unordered_map<std::string, PendingRpc>       pending_;   // keyed by tx_id

    mutable std::mutex                                activeMu_;
    std::string                                       activeSessionId_;

    mutable std::mutex                                eventHandlerMu_;
    EventHandler                                      eventHandler_;
};

}  // namespace sage::bridge
