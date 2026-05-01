#pragma once

#include <atomic>

namespace sage::mcp {
class MCPServer;
}

namespace sage::transport {

// MCP stdio transport: line-delimited JSON-RPC over stdin/stdout.
//
// Reads one JSON-RPC envelope per line from stdin, dispatches to MCPServer,
// writes the response (single-line, no pretty-print) to stdout followed by
// '\n'. Notifications produce no output. Exits cleanly on stdin EOF.
//
// IMPORTANT: stdout is reserved for the JSON-RPC protocol stream — the caller
// (main()) MUST configure spdlog to write to stderr before run() is invoked,
// otherwise log lines will corrupt the protocol stream and break the client.
//
// This is the canonical Anthropic-SDK-compatible MCP transport (no OAuth,
// session-id, protocol-version, or SSE notification handshake — stdio handles
// all of that out-of-band via the Claude Code subprocess lifecycle).
class StdioMcp {
public:
    explicit StdioMcp(mcp::MCPServer& server);

    StdioMcp(const StdioMcp&) = delete;
    StdioMcp& operator=(const StdioMcp&) = delete;
    StdioMcp(StdioMcp&&) = delete;
    StdioMcp& operator=(StdioMcp&&) = delete;

    // Blocks until stdin EOF or stop() called. Single-threaded read loop.
    void run();

    // Sets the running flag to false; the loop exits at the next stdin
    // boundary (next line or EOF). Note: std::getline() blocks on stdin —
    // a true mid-read interrupt requires platform-specific signalling, but
    // for the Claude Code subprocess lifecycle this is sufficient (parent
    // closes stdin on shutdown).
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    mcp::MCPServer& server_;
    std::atomic<bool> running_{false};
};

}  // namespace sage::transport
