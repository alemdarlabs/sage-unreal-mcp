#include "mcp/error_codes.h"
#include "mcp/server.h"
#include "mcp/tool.h"
#include "mcp/tool_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <utility>

using namespace sage::mcp;

namespace {

std::shared_ptr<ToolRegistry> makePingRegistry() {
    auto reg = std::make_shared<ToolRegistry>();
    Tool ping{
        .name        = "ping",
        .description = "echo",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json& p) -> ToolResult {
            nlohmann::json out = {{"pong", true}};
            if (p.is_object() && p.contains("echo")) out["echo"] = p["echo"];
            return out;
        },
    };
    (void)reg->registerTool(std::move(ping));
    return reg;
}

}  // namespace

TEST_CASE("MCPServer responds to initialize", "[mcp][server]") {
    MCPServer server({.name = "sage-test", .version = "0.0.1"}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "initialize"},
        {"id",      1},
        {"params",  nlohmann::json::object()},
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["id"] == 1);
    REQUIRE((*resp)["result"]["serverInfo"]["name"] == "sage-test");
    REQUIRE((*resp)["result"]["capabilities"].contains("tools"));
}

TEST_CASE("MCPServer responds to ping", "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "ping"},
        {"id",      "abc"},
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["id"] == "abc");
    REQUIRE((*resp)["result"].is_object());
}

TEST_CASE("MCPServer lists tools", "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "tools/list"},
        {"id",      2},
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    auto& tools = (*resp)["result"]["tools"];
    REQUIRE(tools.is_array());
    REQUIRE(tools.size() == 1);
    REQUIRE(tools[0]["name"] == "ping");
}

TEST_CASE("MCPServer dispatches tools/call", "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "tools/call"},
        {"id",      3},
        {"params", {
            {"name",      "ping"},
            {"arguments", {{"echo", "hello"}}},
        }},
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["result"]["isError"] == false);
    REQUIRE((*resp)["result"]["structuredContent"]["pong"] == true);
    REQUIRE((*resp)["result"]["structuredContent"]["echo"] == "hello");
}

TEST_CASE("MCPServer returns InvalidParams when tools/call params malformed",
          "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "tools/call"},
        {"id",      4},
        {"params",  nlohmann::json::object()},  // missing 'name'
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["error"]["code"] == static_cast<int>(ErrorCode::InvalidParams));
}

TEST_CASE("MCPServer routes unknown method to MethodNotFound", "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "no/such/method"},
        {"id",      9},
    };
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["error"]["code"] == static_cast<int>(ErrorCode::MethodNotFound));
}

TEST_CASE("MCPServer ignores notification (no response)", "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"method",  "notifications/initialized"},
    };
    auto resp = server.handleRaw(req);
    REQUIRE_FALSE(resp.has_value());
    REQUIRE(server.initialized());
}

TEST_CASE("MCPServer surfaces InvalidRequest for malformed envelope",
          "[mcp][server]") {
    MCPServer server({}, makePingRegistry());
    const nlohmann::json req = nlohmann::json::array();
    auto resp = server.handleRaw(req);
    REQUIRE(resp.has_value());
    REQUIRE((*resp)["id"].is_null());
    REQUIRE((*resp)["error"]["code"] == static_cast<int>(ErrorCode::InvalidRequest));
}
