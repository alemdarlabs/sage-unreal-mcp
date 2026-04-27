#include "transport/http_sse_server.h"

#include "mcp/error_codes.h"
#include "mcp/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <utility>

namespace sage::transport {

HttpSseServer::HttpSseServer(mcp::MCPServer& server, HttpSseConfig cfg)
    : server_(server),
      cfg_(std::move(cfg)),
      http_(std::make_unique<httplib::Server>()) {

    http_->set_read_timeout(cfg_.readTimeoutSec);
    http_->set_write_timeout(cfg_.writeTimeoutSec);

    http_->Post(cfg_.mcpEndpoint,
        [this](const httplib::Request& req, httplib::Response& res) {
            spdlog::trace("HTTP POST {}: {} bytes", cfg_.mcpEndpoint, req.body.size());

            nlohmann::json payload;
            try {
                payload = nlohmann::json::parse(req.body);
            } catch (const std::exception& ex) {
                spdlog::warn("Malformed JSON-RPC body: {}", ex.what());
                const nlohmann::json errResp = {
                    {"jsonrpc", "2.0"},
                    {"id",      nullptr},
                    {"error", {
                        {"code",    static_cast<int>(mcp::ErrorCode::ParseError)},
                        {"message", std::string{"Parse error: "} + ex.what()},
                    }},
                };
                res.status = 400;
                res.set_content(errResp.dump(), "application/json");
                return;
            }

            auto response = server_.handleRaw(payload);
            if (!response) {
                // Notification: no response body per JSON-RPC 2.0.
                res.status = 202;
                return;
            }
            res.status = 200;
            res.set_content(response->dump(), "application/json");
        });

    http_->Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content(R"({"status":"ok"})", "application/json");
    });
}

HttpSseServer::~HttpSseServer() {
    stop();
}

bool HttpSseServer::listen() {
    spdlog::info("HTTP server listening on http://{}:{}{}",
                 cfg_.host, cfg_.port, cfg_.mcpEndpoint);
    running_.store(true);
    const bool ok = http_->listen(cfg_.host, cfg_.port);
    running_.store(false);
    return ok;
}

void HttpSseServer::stop() {
    if (running_.exchange(false)) {
        spdlog::info("HTTP server stopping");
        http_->stop();
    }
}

}  // namespace sage::transport
