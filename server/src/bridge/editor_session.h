#pragma once

#include <chrono>
#include <cstdint>
#include <string>

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
    std::int64_t pid = 0;

    std::chrono::system_clock::time_point connected_at;
    std::chrono::system_clock::time_point last_heartbeat;
};

}  // namespace sage::bridge
