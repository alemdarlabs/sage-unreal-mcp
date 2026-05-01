#pragma once

#include "bridge/editor_session.h"
#include "bridge/protocol.h"
#include "mcp/tool.h"  // mcp::ToolResult, mcp::ErrorObject

#include <atomic>
#include <chrono>
#include <condition_variable>
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
// Multi-editor routing (slot-aware, per-call) — ADR-004 §2 + Milestone 1.5b:
// `dispatchTool` accepts an optional target id/label; when empty, falls back to
// the active session pointer; if no active and exactly one editor connected,
// uses that one implicitly; if multiple are connected without an active, returns
// EditorNotConnected with an "ambiguous target" message.
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

    // Synchronous tool dispatch with explicit target editor (ADR-004 §2).
    // `targetIdOrLabel` may be a session_id, label, or instance_id; empty
    // means "use the active session pointer (or the only connected editor)".
    [[nodiscard]] mcp::ToolResult dispatchTool(std::string_view tool,
                                                const nlohmann::json& args,
                                                std::chrono::milliseconds timeout,
                                                std::string_view targetIdOrLabel);

    // Convenience: target=active, caller-supplied timeout.
    [[nodiscard]] mcp::ToolResult dispatchTool(std::string_view tool,
                                                const nlohmann::json& args,
                                                std::chrono::milliseconds timeout);

    // Convenience: target=active, default timeout.
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

    // Session lifecycle hook — invoked off the bridge worker thread on each
    // hello-completed (kind="connected") and connection-close (kind="disconnected").
    // For "disconnected" the session struct is the snapshot taken just before
    // erase so callers see the final identity. main.cpp wires this to the
    // MCP notification publisher.
    using SessionEventCallback = std::function<void(std::string_view kind,
                                                     const EditorSession& session)>;
    void setSessionEventCallback(SessionEventCallback cb);

    // Block until a matching editor session exists, or timeout. If
    // `slot_id_filter` is set, waits for that specific slot; otherwise wakes
    // on the first session that arrives. Returns nullopt on timeout.
    //
    // Caller-side fast path (already-connected) is NOT done here — the tool
    // handler checks snapshotSessions() first to avoid a lock + cv round-trip.
    [[nodiscard]] std::optional<EditorSession> waitForSession(
        std::optional<std::string> slot_id_filter,
        std::chrono::milliseconds timeout);

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
        // Originating session id — set on dispatch, read on close to fail-fast
        // pending RPCs whose target editor disconnected before the result
        // arrived (otherwise they'd time out at the configured deadline).
        std::string                   session_id;
    };

    BridgeConfig                                      cfg_;
    std::unique_ptr<ix::WebSocketServer>              server_;
    std::atomic<bool>                                 running_{false};
    std::atomic<std::uint64_t>                        txCounter_{0};

    mutable std::mutex                                sessionsMu_;
    std::condition_variable                           sessionsCv_;  // signals on hello + close
    std::unordered_map<std::string, EditorSession>    sessions_;    // keyed by ConnectionState id

    mutable std::mutex                                pendingMu_;
    std::unordered_map<std::string, PendingRpc>       pending_;   // keyed by tx_id

    mutable std::mutex                                activeMu_;
    std::string                                       activeSessionId_;

    mutable std::mutex                                eventHandlerMu_;
    EventHandler                                      eventHandler_;

    mutable std::mutex                                sessionEventMu_;
    SessionEventCallback                              sessionEventCb_;
};

}  // namespace sage::bridge
