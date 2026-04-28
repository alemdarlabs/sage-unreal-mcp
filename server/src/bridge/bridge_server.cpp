#include "bridge/bridge_server.h"

#include "mcp/error_codes.h"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
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

        // Resolve any in-flight RPCs so awaiting threads don't hang on shutdown.
        std::lock_guard lk(pendingMu_);
        for (auto& [tx_id, rpc] : pending_) {
            try {
                rpc.promise.set_value(std::unexpected(
                    mcp::ErrorObject::fromCode(mcp::ErrorCode::InternalError,
                                                "bridge stopped")));
            } catch (const std::future_error&) {
                // promise already satisfied — ignore
            }
        }
        pending_.clear();
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

std::optional<EditorSession> BridgeServer::snapshotSession(std::string_view sessionId) const {
    std::lock_guard lk(sessionsMu_);
    if (auto it = sessions_.find(std::string{sessionId}); it != sessions_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<EditorSession> BridgeServer::findByIdOrLabel(std::string_view idOrLabel) const {
    std::lock_guard lk(sessionsMu_);
    if (auto it = sessions_.find(std::string{idOrLabel}); it != sessions_.end()) {
        return it->second;
    }
    for (const auto& [_, s] : sessions_) {
        if (s.label == idOrLabel) return s;
        if (s.instance_id == idOrLabel) return s;
    }
    return std::nullopt;
}

void BridgeServer::setActiveSession(std::string sessionId) {
    std::lock_guard lk(activeMu_);
    activeSessionId_ = std::move(sessionId);
}

std::string BridgeServer::activeSession() const {
    std::lock_guard lk(activeMu_);
    return activeSessionId_;
}

std::size_t BridgeServer::pendingCount() const {
    std::lock_guard lk(pendingMu_);
    return pending_.size();
}

mcp::ToolResult BridgeServer::dispatchTool(std::string_view tool,
                                            const nlohmann::json& args) {
    return dispatchTool(tool, args, cfg_.defaultDispatchTimeout);
}

mcp::ToolResult BridgeServer::dispatchTool(std::string_view tool,
                                            const nlohmann::json& args,
                                            std::chrono::milliseconds timeout) {
    if (!running_.load()) {
        return std::unexpected(mcp::ErrorObject::fromCode(
            mcp::ErrorCode::InternalError, "bridge not running"));
    }

    auto clients = server_->getClients();
    if (clients.empty()) {
        return std::unexpected(mcp::ErrorObject::fromCode(
            mcp::ErrorCode::EditorNotConnected, "no plugin connected"));
    }
    auto ws = *clients.begin();

    const std::string txId = nextTxId();
    std::future<mcp::ToolResult> future;
    {
        std::lock_guard lk(pendingMu_);
        future = pending_[txId].promise.get_future();
    }

    const auto envelope = toolCallMessage(txId, std::string{tool}, args);
    ws->send(envelope.dump());
    spdlog::debug("Bridge: dispatched tool='{}' tx={}", tool, txId);

    if (future.wait_for(timeout) != std::future_status::ready) {
        std::lock_guard lk(pendingMu_);
        pending_.erase(txId);
        spdlog::warn("Bridge: tool '{}' (tx={}) timed out after {}ms",
                     tool, txId, timeout.count());
        return std::unexpected(mcp::ErrorObject::fromCode(
            mcp::ErrorCode::InternalError, "tool dispatch timeout"));
    }
    return future.get();
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
        spdlog::debug("Bridge: incoming type='{}' from id={}", type, session_id);
        switch (parseType(type)) {
        case MessageType::Hello:      handleHello(ws, session_id, parsed); break;
        case MessageType::Heartbeat:  handleHeartbeat(ws, session_id, parsed); break;
        case MessageType::ToolResult: handleToolResult(session_id, parsed); break;
        case MessageType::Event:    handleEvent(session_id, parsed); break;
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

void BridgeServer::handleToolResult(const std::string& session_id, const Json& payload) {
    auto resRes = parseToolResult(payload);
    if (!resRes.has_value()) {
        spdlog::warn("Bridge: bad tool_result from id={}: {}", session_id, resRes.error());
        return;
    }
    const auto& r = *resRes;

    std::lock_guard lk(pendingMu_);
    auto it = pending_.find(r.tx_id);
    if (it == pending_.end()) {
        spdlog::warn("Bridge: tool_result for unknown tx_id={}", r.tx_id);
        return;
    }

    try {
        if (r.success) {
            it->second.promise.set_value(r.result.value_or(nlohmann::json::object()));
        } else {
            mcp::ErrorObject err;
            if (r.error.has_value() && r.error->is_object()) {
                const int code = r.error->value("code",
                                                static_cast<int>(mcp::ErrorCode::GenericSage));
                err.code    = static_cast<mcp::ErrorCode>(code);
                err.message = r.error->value("message", "tool failed");
            } else {
                err = mcp::ErrorObject::fromCode(mcp::ErrorCode::GenericSage, "tool failed");
            }
            it->second.promise.set_value(std::unexpected(std::move(err)));
        }
    } catch (const std::future_error&) {
        // Promise already satisfied (e.g. timeout fired) — drop result.
        spdlog::debug("Bridge: late tool_result for tx={}; promise already satisfied", r.tx_id);
    }
    pending_.erase(it);
}

void BridgeServer::handleEvent(const std::string& session_id, const Json& payload) {
    spdlog::info("Bridge: event recv from id={} kind='{}'",
                 session_id, payload.value("kind", "<missing>"));
    auto evRes = parseEvent(payload);
    if (!evRes.has_value()) {
        spdlog::warn("Bridge: event parse failed (id={}): {}", session_id, evRes.error());
        return;
    }

    // Resolve slot_id from the originating session — plugins don't echo it
    // on every event, the bridge already knows.
    std::string slotId;
    {
        std::lock_guard lk(sessionsMu_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            spdlog::debug("Bridge: event from unregistered session id={}, dropping", session_id);
            return;
        }
        slotId = it->second.slot_id;
    }

    EventHandler h;
    {
        std::lock_guard lk(eventHandlerMu_);
        h = eventHandler_;
    }
    if (!h) {
        spdlog::debug("Bridge: event handler not set; dropping kind='{}'", evRes->kind);
        return;
    }
    try {
        h(slotId, *evRes);
    } catch (const std::exception& ex) {
        spdlog::error("Bridge: event handler threw on kind='{}': {}", evRes->kind, ex.what());
    }
}

void BridgeServer::setEventHandler(EventHandler handler) {
    std::lock_guard lk(eventHandlerMu_);
    eventHandler_ = std::move(handler);
}

std::string BridgeServer::nextTxId() {
    const auto n = txCounter_.fetch_add(1, std::memory_order_relaxed);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "tx-%016llx", static_cast<unsigned long long>(n));
    return std::string{buf};
}

}  // namespace sage::bridge
