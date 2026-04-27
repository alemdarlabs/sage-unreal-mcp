#include "bridge/bridge_server.h"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace sage::bridge {

BridgeServer::BridgeServer(BridgeConfig cfg)
    : cfg_(std::move(cfg)),
      server_(std::make_unique<ix::WebSocketServer>(cfg_.port, cfg_.host)) {

    server_->setOnClientMessageCallback(
        [this](const std::shared_ptr<ix::ConnectionState>& state,
               ix::WebSocket& ws,
               const ix::WebSocketMessagePtr& msg) {
            onClientMessage(state, ws, msg);
        });
}

BridgeServer::~BridgeServer() {
    stop();
}

bool BridgeServer::start() {
    auto [ok, err] = server_->listen();
    if (!ok) {
        spdlog::error("Bridge: listen({}:{}) failed: {}", cfg_.host, cfg_.port, err);
        return false;
    }
    server_->start();
    running_.store(true);
    spdlog::info("Bridge listening on ws://{}:{}{}", cfg_.host, cfg_.port, cfg_.endpoint);
    return true;
}

void BridgeServer::stop() {
    if (running_.exchange(false)) {
        spdlog::info("Bridge: stopping");
        server_->stop();
    }
}

std::vector<EditorSession> BridgeServer::snapshotSessions() const {
    std::lock_guard lk(sessionsMu_);
    std::vector<EditorSession> out;
    out.reserve(sessions_.size());
    for (const auto& [_, s] : sessions_) {
        out.push_back(s);
    }
    return out;
}

std::size_t BridgeServer::sessionCount() const {
    std::lock_guard lk(sessionsMu_);
    return sessions_.size();
}

void BridgeServer::onClientMessage(const std::shared_ptr<ix::ConnectionState>& state,
                                    ix::WebSocket& ws,
                                    const ix::WebSocketMessagePtr& msg) {
    const std::string session_id = state->getId();
    const std::string remote     = state->getRemoteIp();

    switch (msg->type) {
    case ix::WebSocketMessageType::Open:
        spdlog::info("Bridge: connection opened from {} (id={})", remote, session_id);
        break;

    case ix::WebSocketMessageType::Close:
        spdlog::info("Bridge: connection closed (id={}, code={}, reason='{}')",
                     session_id, msg->closeInfo.code, msg->closeInfo.reason);
        {
            std::lock_guard lk(sessionsMu_);
            sessions_.erase(session_id);
        }
        break;

    case ix::WebSocketMessageType::Error:
        spdlog::warn("Bridge: connection error (id={}): {}",
                     session_id, msg->errorInfo.reason);
        break;

    case ix::WebSocketMessageType::Message: {
        Json parsed;
        try {
            parsed = Json::parse(msg->str);
        } catch (const std::exception& ex) {
            spdlog::warn("Bridge: malformed JSON from id={}: {}", session_id, ex.what());
            ws.send(errorMessage(-32700, "parse error").dump());
            return;
        }
        if (!parsed.is_object() || !parsed.contains("type") || !parsed["type"].is_string()) {
            spdlog::warn("Bridge: missing 'type' field from id={}", session_id);
            ws.send(errorMessage(-32600, "missing 'type' field").dump());
            return;
        }
        const auto type = parsed["type"].get<std::string>();
        switch (parseType(type)) {
        case MessageType::Hello:      handleHello(ws, session_id, parsed); break;
        case MessageType::Heartbeat:  handleHeartbeat(ws, session_id, parsed); break;
        case MessageType::ToolResult: handleToolResult(session_id, parsed); break;
        case MessageType::Event:
            spdlog::debug("Bridge: event received from id={} (deferred to Phase 2)", session_id);
            break;
        default:
            spdlog::warn("Bridge: unknown message type '{}' from id={}", type, session_id);
            ws.send(errorMessage(-32601, "unknown type").dump());
            break;
        }
        break;
    }

    case ix::WebSocketMessageType::Ping:
    case ix::WebSocketMessageType::Pong:
    case ix::WebSocketMessageType::Fragment:
        // ixwebsocket handles ping/pong internally for keepalive; ours is
        // application-level via `heartbeat` message.
        break;
    }
}

void BridgeServer::handleHello(ix::WebSocket& ws,
                                const std::string& session_id,
                                const Json& payload) {
    auto helloRes = parseHello(payload);
    if (!helloRes.has_value()) {
        spdlog::warn("Bridge: hello parse failed (id={}): {}", session_id, helloRes.error());
        ws.send(errorMessage(-32602, "invalid hello: " + helloRes.error()).dump());
        return;
    }
    const HelloMessage& hello = *helloRes;

    EditorSession s;
    s.session_id     = session_id;
    s.slot_id        = hello.slot_id;
    s.connected_at   = std::chrono::system_clock::now();
    s.last_heartbeat = s.connected_at;

    if (hello.editor.is_object()) {
        s.instance_id    = hello.editor.value("id", "");
        s.label          = hello.editor.value("label", "");
        s.project_id     = hello.editor.value("project_id", "");
        s.project_path   = hello.editor.value("project_path", "");
        s.engine_version = hello.editor.value("engine_version", "");
        s.pid            = hello.editor.value("pid", static_cast<std::int64_t>(0));
    }

    {
        std::lock_guard lk(sessionsMu_);
        sessions_[session_id] = s;
    }

    spdlog::info(
        "Bridge handshake: slot={}, label='{}', project='{}', instance='{}', engine={}",
        s.slot_id, s.label, s.project_path, s.instance_id, s.engine_version);

    ws.send(welcomeMessage(session_id, "0.1.0").dump());
}

void BridgeServer::handleHeartbeat(ix::WebSocket& ws,
                                    const std::string& session_id,
                                    const Json& payload) {
    auto hbRes = parseHeartbeat(payload);
    if (!hbRes.has_value()) {
        ws.send(errorMessage(-32602, "invalid heartbeat: " + hbRes.error()).dump());
        return;
    }
    {
        std::lock_guard lk(sessionsMu_);
        if (auto it = sessions_.find(session_id); it != sessions_.end()) {
            it->second.last_heartbeat = std::chrono::system_clock::now();
        }
    }
    spdlog::trace("Bridge heartbeat from id={}", session_id);
    ws.send(heartbeatAckMessage(hbRes->ts).dump());
}

void BridgeServer::handleToolResult(const std::string& session_id, const Json& /*payload*/) {
    // Milestone 1.3b: route tool_result to pending RPC promise table.
    spdlog::debug("Bridge: tool_result received (id={}) — handler pending Milestone 1.3b",
                  session_id);
}

}  // namespace sage::bridge
