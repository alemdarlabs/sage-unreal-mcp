#include "transport/stdio_mcp.h"

#include "mcp/error_codes.h"
#include "mcp/server.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

namespace sage::transport {

StdioMcp::StdioMcp(mcp::MCPServer& server) : server_(server) {}

void StdioMcp::run() {
    running_.store(true);
    spdlog::info("MCP stdio transport: reading JSON-RPC from stdin "
                 "(line-delimited, response on stdout)");

    // Disable stdin buffering pathologies: line-delimited reads are how MCP
    // stdio transport is specified, and most stdlibs do this anyway. We
    // explicitly avoid sync_with_stdio adjustments — the parent (Claude Code)
    // controls stream attributes via the spawned pipe.
    std::string line;
    while (running_.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(line);
        } catch (const std::exception& ex) {
            const nlohmann::json errResp = {
                {"jsonrpc", "2.0"},
                {"id",      nullptr},
                {"error", {
                    {"code",    static_cast<int>(mcp::ErrorCode::ParseError)},
                    {"message", std::string{"Parse error: "} + ex.what()},
                }},
            };
            std::cout << errResp.dump() << '\n' << std::flush;
            continue;
        }

        auto response = server_.handleRaw(payload);
        if (response) {
            // Single line; explicit flush so Claude Code receives the response
            // immediately rather than buffering until next line.
            std::cout << response->dump() << '\n' << std::flush;
        }
        // Notification: no response per JSON-RPC 2.0.
    }

    running_.store(false);
    spdlog::info("MCP stdio transport: stdin closed, shutting down");
}

void StdioMcp::stop() {
    running_.store(false);
}

}  // namespace sage::transport
