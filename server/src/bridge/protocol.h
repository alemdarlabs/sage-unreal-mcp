#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

// Plugin↔Server bridge wire protocol. JSON over WebSocket. Top-level envelope
// dispatched on the `type` string field. See ADR-015.

namespace sage::bridge {

using Json = nlohmann::json;

enum class MessageType {
    // Plugin → Server
    Hello,
    Heartbeat,
    ToolResult,
    Event,
    // Server → Plugin
    Welcome,
    HeartbeatAck,
    ToolCall,
    Error,

    Unknown,
};

[[nodiscard]] MessageType parseType(std::string_view type) noexcept;
[[nodiscard]] const char* toString(MessageType type) noexcept;

// ---- Plugin → Server payloads -----------------------------------------------

struct HelloMessage {
    std::string version;
    std::string slot_id;
    Json editor = Json::object();  // raw editor identity (api-spec.md §Handshake)
    std::optional<std::string> asset_registry_hash;
};

struct HeartbeatMessage {
    std::int64_t ts = 0;
};

struct ToolResultMessage {
    std::string tx_id;
    bool success = true;
    std::optional<Json> result;
    std::optional<Json> error;
    std::optional<std::string> before_hash;
    std::optional<std::string> after_hash;
};

[[nodiscard]] std::expected<HelloMessage, std::string>      parseHello(const Json& j);
[[nodiscard]] std::expected<HeartbeatMessage, std::string>  parseHeartbeat(const Json& j);
[[nodiscard]] std::expected<ToolResultMessage, std::string> parseToolResult(const Json& j);

// ---- Server → Plugin payloads (factories) -----------------------------------

[[nodiscard]] Json welcomeMessage(const std::string& session_id,
                                   const std::string& server_version);
[[nodiscard]] Json heartbeatAckMessage(std::int64_t ts);
[[nodiscard]] Json toolCallMessage(const std::string& tx_id,
                                    const std::string& tool,
                                    const Json& args);
[[nodiscard]] Json errorMessage(int code, const std::string& message);

}  // namespace sage::bridge
