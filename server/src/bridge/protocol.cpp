#include "bridge/protocol.h"

#include <unordered_map>

namespace sage::bridge {

MessageType parseType(std::string_view type) noexcept {
    static const std::unordered_map<std::string_view, MessageType> kMap = {
        {"hello",         MessageType::Hello},
        {"heartbeat",     MessageType::Heartbeat},
        {"tool_result",   MessageType::ToolResult},
        {"event",         MessageType::Event},
        {"welcome",       MessageType::Welcome},
        {"heartbeat_ack", MessageType::HeartbeatAck},
        {"tool_call",     MessageType::ToolCall},
        {"error",         MessageType::Error},
    };
    const auto it = kMap.find(type);
    return it == kMap.end() ? MessageType::Unknown : it->second;
}

const char* toString(MessageType type) noexcept {
    switch (type) {
    case MessageType::Hello:        return "hello";
    case MessageType::Heartbeat:    return "heartbeat";
    case MessageType::ToolResult:   return "tool_result";
    case MessageType::Event:        return "event";
    case MessageType::Welcome:      return "welcome";
    case MessageType::HeartbeatAck: return "heartbeat_ack";
    case MessageType::ToolCall:     return "tool_call";
    case MessageType::Error:        return "error";
    case MessageType::Unknown:      return "unknown";
    }
    return "unknown";
}

std::expected<HelloMessage, std::string> parseHello(const Json& j) {
    if (!j.is_object()) return std::unexpected("hello: not an object");
    if (!j.contains("version") || !j["version"].is_string())
        return std::unexpected("hello: missing or non-string 'version'");
    if (!j.contains("slot_id") || !j["slot_id"].is_string())
        return std::unexpected("hello: missing or non-string 'slot_id'");

    HelloMessage h;
    h.version = j["version"].get<std::string>();
    h.slot_id = j["slot_id"].get<std::string>();
    if (j.contains("editor") && j["editor"].is_object()) {
        h.editor = j["editor"];
    }
    if (j.contains("asset_registry_hash") && j["asset_registry_hash"].is_string()) {
        h.asset_registry_hash = j["asset_registry_hash"].get<std::string>();
    }
    return h;
}

std::expected<HeartbeatMessage, std::string> parseHeartbeat(const Json& j) {
    HeartbeatMessage h;
    if (j.contains("ts")) {
        const auto& ts = j["ts"];
        if (ts.is_number_integer())      h.ts = ts.get<std::int64_t>();
        else if (ts.is_number_unsigned()) h.ts = static_cast<std::int64_t>(ts.get<std::uint64_t>());
        else if (ts.is_number_float())    h.ts = static_cast<std::int64_t>(ts.get<double>());
    }
    return h;
}

std::expected<EventMessage, std::string> parseEvent(const Json& j) {
    if (!j.is_object())
        return std::unexpected("event: not an object");
    if (!j.contains("kind") || !j["kind"].is_string())
        return std::unexpected("event: missing or non-string 'kind'");

    EventMessage e;
    e.kind = j["kind"].get<std::string>();
    if (j.contains("payload")) e.payload = j["payload"];
    else                       e.payload = Json::object();
    return e;
}

std::expected<ToolResultMessage, std::string> parseToolResult(const Json& j) {
    if (!j.contains("tx_id") || !j["tx_id"].is_string())
        return std::unexpected("tool_result: missing or non-string 'tx_id'");

    ToolResultMessage r;
    r.tx_id = j["tx_id"].get<std::string>();
    r.success = j.value("success", true);
    if (j.contains("result"))     r.result = j["result"];
    if (j.contains("error"))      r.error  = j["error"];
    if (j.contains("before_hash") && j["before_hash"].is_string())
        r.before_hash = j["before_hash"].get<std::string>();
    if (j.contains("after_hash") && j["after_hash"].is_string())
        r.after_hash = j["after_hash"].get<std::string>();
    return r;
}

Json welcomeMessage(const std::string& session_id, const std::string& server_version) {
    return Json{
        {"type",           "welcome"},
        {"session_id",     session_id},
        {"server_version", server_version},
    };
}

Json heartbeatAckMessage(std::int64_t ts) {
    return Json{
        {"type", "heartbeat_ack"},
        {"ts",   ts},
    };
}

Json toolCallMessage(const std::string& tx_id,
                     const std::string& tool,
                     const Json& args) {
    return Json{
        {"type",  "tool_call"},
        {"tx_id", tx_id},
        {"tool",  tool},
        {"args",  args},
    };
}

Json errorMessage(int code, const std::string& message) {
    return Json{
        {"type",    "error"},
        {"code",    code},
        {"message", message},
    };
}

}  // namespace sage::bridge
