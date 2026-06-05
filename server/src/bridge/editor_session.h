#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ix { class WebSocket; }

namespace sage::bridge {

// A connected editor instance. Created on `hello` handshake, removed on close.
// Thread-safety: BridgeServer holds these under a mutex.
struct EditorSession {
    std::string session_id;       // server-assigned, derived from the WS connection
    std::string slot_id;          // ADR-003 + ADR-014 (Blake3)
    std::string instance_id;      // editor-provided "{ProjectName}@{ShortSession}"
    std::string label;            // host / client / experiment / ...
    std::string project_id;
    std::string project_path;
    std::string engine_version;   // full "5.7.4" for diagnostics
    std::string plugin_version;   // SageBridge VersionName from the plugin descriptor
    std::int64_t pid = 0;

    std::chrono::system_clock::time_point connected_at;
    std::chrono::system_clock::time_point last_heartbeat;

    // Raw pointer to the bridge WebSocket; lifetime managed by ix::WebSocketServer.
    // Cleared via close-callback erase before the underlying object is destroyed.
    // Used by dispatchTool() to route per-call to a specific editor (ADR-004 §2).
    ix::WebSocket* ws = nullptr;
};

}  // namespace sage::bridge
