#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace sage::mcp {
class MCPServer;
}

namespace sage::transport {

struct HttpSseConfig {
    std::string host{"127.0.0.1"};
    int port{7777};
    std::string mcpEndpoint{"/mcp"};
    int readTimeoutSec{30};
    int writeTimeoutSec{30};
};

// HTTP transport for Sage MCP. POST `/mcp` accepts a JSON-RPC 2.0 envelope
// and returns:
//   - 200 application/json with response body for requests, or
//   - 202 (empty body) for notifications.
//
// Phase 1 ships single-shot JSON. SSE chunked streaming will be layered on the
// same endpoint when streaming tools (compile, indexing) arrive — the choice
// of cpp-httplib was made specifically because it supports
// `set_chunked_content_provider` for that future expansion.
class HttpSseServer {
public:
    HttpSseServer(mcp::MCPServer& server, HttpSseConfig cfg);
    ~HttpSseServer();

    HttpSseServer(const HttpSseServer&) = delete;
    HttpSseServer& operator=(const HttpSseServer&) = delete;
    HttpSseServer(HttpSseServer&&) = delete;
    HttpSseServer& operator=(HttpSseServer&&) = delete;

    // Blocks until stop() is called or bind fails. Returns false on bind failure.
    [[nodiscard]] bool listen();
    void stop();

    [[nodiscard]] const HttpSseConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    mcp::MCPServer& server_;
    HttpSseConfig cfg_;
    std::unique_ptr<httplib::Server> http_;
    std::atomic<bool> running_{false};
};

}  // namespace sage::transport
