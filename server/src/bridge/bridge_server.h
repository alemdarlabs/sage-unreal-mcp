#pragma once

#include "bridge/editor_session.h"
#include "bridge/protocol.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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
};

// WebSocket bridge between Sage server and N Unreal Editor plugins.
//
// Lifecycle:
//   1. Plugin connects (TCP + WS upgrade) → state = HANDSHAKING
//   2. Plugin sends `hello` → server validates, registers session, replies `welcome`
//   3. Heartbeat exchange (15s default per ADR-001 / api-spec.md)
//   4. (Milestone 1.3b) tool_call ⇄ tool_result dispatch
//   5. Close → server prunes session
//
// Thread model: ixwebsocket spins worker threads; callbacks fire there. The
// session map is mutex-protected; snapshot helpers copy under lock.
class BridgeServer {
public:
    explicit BridgeServer(BridgeConfig cfg);
    ~BridgeServer();

    BridgeServer(const BridgeServer&) = delete;
    BridgeServer& operator=(const BridgeServer&) = delete;

    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] std::vector<EditorSession> snapshotSessions() const;
    [[nodiscard]] std::size_t sessionCount() const;
    [[nodiscard]] const BridgeConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    void onClientMessage(const std::shared_ptr<ix::ConnectionState>& state,
                         ix::WebSocket& ws,
                         const ix::WebSocketMessagePtr& msg);

    void handleHello(ix::WebSocket& ws, const std::string& session_id, const Json& payload);
    void handleHeartbeat(ix::WebSocket& ws, const std::string& session_id, const Json& payload);
    void handleToolResult(const std::string& session_id, const Json& payload);

    BridgeConfig                                      cfg_;
    std::unique_ptr<ix::WebSocketServer>              server_;
    std::atomic<bool>                                 running_{false};

    mutable std::mutex                                sessionsMu_;
    std::unordered_map<std::string, EditorSession>   sessions_;  // keyed by ConnectionState id
};

}  // namespace sage::bridge
