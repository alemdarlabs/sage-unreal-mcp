#include "bridge/bridge_server.h"

#include "mcp/error_codes.h"
#include "version.h"

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>
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
        {
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

        // Wake any waitForSession() callers so they fall through to nullopt
        // instead of waiting out their timeout.
        sessionsCv_.notify_all();
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
    // 1. session_id direct hit (map key)
    if (auto it = sessions_.find(std::string{idOrLabel}); it != sessions_.end()) {
        return it->second;
    }
    // 2. label, full instance_id, or project-name prefix of instance_id
    //    (e.g. "Kale" matches instance_id "Kale@83610251" — friendlier than
    //    forcing callers to know the per-restart hash suffix).
    for (const auto& [_, s] : sessions_) {
        if (s.label == idOrLabel) return s;
        if (s.instance_id == idOrLabel) return s;
        const auto atPos = s.instance_id.find('@');
        if (atPos != std::string::npos &&
            std::string_view{s.instance_id}.substr(0, atPos) == idOrLabel) {
            return s;
        }
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
    return dispatchTool(tool, args, cfg_.defaultDispatchTimeout, std::string_view{});
}

mcp::ToolResult BridgeServer::dispatchTool(std::string_view tool,
                                            const nlohmann::json& args,
                                            std::chrono::milliseconds timeout) {
    return dispatchTool(tool, args, timeout, std::string_view{});
}

mcp::ToolResult BridgeServer::dispatchTool(std::string_view tool,
                                            const nlohmann::json& args,
                                            std::chrono::milliseconds timeout,
                                            std::string_view targetIdOrLabel) {
    if (!running_.load()) {
        return std::unexpected(mcp::ErrorObject::fromCode(
            mcp::ErrorCode::InternalError, "bridge not running"));
    }

    // Resolve target editor (ADR-004 §2):
    //   1. explicit targetIdOrLabel → findByIdOrLabel
    //   2. fallback to active session pointer
    //   3. fallback to the only connected editor
    //   4. error if multiple connected and no preference set
    std::string targetSessionId;
    if (!targetIdOrLabel.empty()) {
        auto session = findByIdOrLabel(targetIdOrLabel);
        if (!session.has_value()) {
            std::string msg = "no editor session matches '";
            msg += std::string{targetIdOrLabel};
            msg += "' (use list_editors)";
            return std::unexpected(mcp::ErrorObject::fromCode(
                mcp::ErrorCode::EditorNotConnected, std::move(msg)));
        }
        targetSessionId = session->session_id;
    } else {
        targetSessionId = activeSession();
        if (targetSessionId.empty()) {
            const auto sessions = snapshotSessions();
            if (sessions.empty()) {
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::EditorNotConnected, "no plugin connected"));
            }
            if (sessions.size() == 1) {
                targetSessionId = sessions[0].session_id;
            } else {
                return std::unexpected(mcp::ErrorObject::fromCode(
                    mcp::ErrorCode::EditorNotConnected,
                    "multiple editors connected and no active one set; "
                    "pass _editor or call set_active_editor"));
            }
        }
    }

    // Look up the WebSocket for this session.
    ix::WebSocket* targetWs = nullptr;
    {
        std::lock_guard lk(sessionsMu_);
        auto it = sessions_.find(targetSessionId);
        if (it != sessions_.end()) {
            targetWs = it->second.ws;
        }
    }
    if (targetWs == nullptr) {
        // Session erased between resolution and lookup, or ws never recorded.
        return std::unexpected(mcp::ErrorObject::fromCode(
            mcp::ErrorCode::EditorNotConnected,
            "target editor session no longer connected"));
    }

    const std::string txId = nextTxId();
    std::future<mcp::ToolResult> future;
    {
        std::lock_guard lk(pendingMu_);
        auto& rpc      = pending_[txId];
        rpc.session_id = targetSessionId;
        future         = rpc.promise.get_future();
    }

    const auto envelope = toolCallMessage(txId, std::string{tool}, args);
    targetWs->send(envelope.dump());
    spdlog::debug("Bridge: dispatched tool='{}' tx={} target={}",
                  tool, txId, targetSessionId);

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

    case ix::WebSocketMessageType::Close: {
        spdlog::info("Bridge: connection closed (id={}, code={}, reason='{}')",
                     session_id, msg->closeInfo.code, msg->closeInfo.reason);

        // Snapshot the session struct before erasing so the disconnect
        // callback sees the final identity (slot_id, label, instance_id).
        std::optional<EditorSession> snapshot;
        {
            std::lock_guard lk(sessionsMu_);
            if (auto it = sessions_.find(session_id); it != sessions_.end()) {
                snapshot = it->second;
                sessions_.erase(it);
            }
        }

        // Mirror the connect ordering: publish "disconnected" BEFORE
        // resolving pending RPCs and notifying the cv. Pending RPC
        // resolution will write tool-error responses to stdout; if we did
        // those first the agent would see error responses before the
        // disconnected notification that explains them.
        if (snapshot) {
            SessionEventCallback cb;
            {
                std::lock_guard lk(sessionEventMu_);
                cb = sessionEventCb_;
            }
            if (cb) {
                try { cb("disconnected", *snapshot); }
                catch (const std::exception& ex) {
                    spdlog::warn(
                        "Bridge: session event callback threw on disconnected: {}", ex.what());
                }
            }
        }

        // Fail-fast every pending RPC targeted at this session — otherwise
        // each one would sit on its 30s timeout, surfacing to the MCP client
        // as a generic "tool dispatch timeout" (Claude harness then renders
        // those as 'user reject', which is what HeroFlight saw).
        std::size_t failed = 0;
        {
            std::lock_guard lk(pendingMu_);
            for (auto it = pending_.begin(); it != pending_.end(); ) {
                if (it->second.session_id == session_id) {
                    try {
                        it->second.promise.set_value(std::unexpected(
                            mcp::ErrorObject::fromCode(mcp::ErrorCode::EditorNotConnected,
                                "editor disconnected before tool result")));
                    } catch (const std::future_error&) {
                        // Already satisfied — ignore.
                    }
                    it = pending_.erase(it);
                    ++failed;
                } else {
                    ++it;
                }
            }
        }
        if (failed > 0) {
            spdlog::info("Bridge: failed-fast {} pending RPC(s) on session {} close",
                         failed, session_id);
        }

        sessionsCv_.notify_all();
        break;
    }

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
    s.plugin_version = hello.plugin_version;
    s.connected_at   = std::chrono::system_clock::now();
    s.last_heartbeat = s.connected_at;
    s.ws             = &ws;  // ADR-004 §2: per-call routing needs ws-by-session lookup.

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
        "Bridge handshake: slot={}, label='{}', project='{}', instance='{}', engine={}, plugin={}",
        s.slot_id, s.label, s.project_path, s.instance_id, s.engine_version, s.plugin_version);

    // Fire the "connected" callback BEFORE signaling the cv. The callback
    // typically publishes a notifications/message envelope on stdout; if we
    // notified first, the wait_for_editor waiter could wake, build a tool
    // response, and write that response to stdout before the notification
    // arrived — leaving observed order as response⇒notification, which makes
    // the notification useless for "agent learns the editor is up" UX. The
    // callback returns quickly (lock-free queue write into the stdio mutex);
    // this small reordering is safe.
    SessionEventCallback cb;
    {
        std::lock_guard lk(sessionEventMu_);
        cb = sessionEventCb_;
    }
    if (cb) {
        try { cb("connected", s); }
        catch (const std::exception& ex) {
            spdlog::warn("Bridge: session event callback threw on connected: {}", ex.what());
        }
    }

    sessionsCv_.notify_all();  // wake wait_for_editor — AFTER notification published

    ws.send(welcomeMessage(session_id, std::string{sage::kServerVersion}).dump());
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

void BridgeServer::setSessionEventCallback(SessionEventCallback cb) {
    std::lock_guard lk(sessionEventMu_);
    sessionEventCb_ = std::move(cb);
}

std::optional<EditorSession> BridgeServer::waitForSession(
    std::optional<std::string> slot_id_filter,
    std::chrono::milliseconds timeout) {

    auto matches = [&](const EditorSession& s) {
        return !slot_id_filter || s.slot_id == *slot_id_filter;
    };

    std::unique_lock lk(sessionsMu_);
    // Predicate also covers the already-connected case: cv.wait_for returns
    // true immediately if matches() is already satisfied. Caller can still
    // do a snapshot fast path before getting here to avoid the lock churn.
    const bool ok = sessionsCv_.wait_for(lk, timeout, [&] {
        if (!running_.load()) return true;  // shutdown — break out
        for (const auto& [_, s] : sessions_) {
            if (matches(s)) return true;
        }
        return false;
    });
    if (!ok) return std::nullopt;
    if (!running_.load()) return std::nullopt;
    for (const auto& [_, s] : sessions_) {
        if (matches(s)) return s;
    }
    return std::nullopt;  // race: matched session disconnected before we re-checked
}

std::string BridgeServer::nextTxId() {
    const auto n = txCounter_.fetch_add(1, std::memory_order_relaxed);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "tx-%016llx", static_cast<unsigned long long>(n));
    return std::string{buf};
}

}  // namespace sage::bridge
