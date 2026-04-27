#include "mcp/error_codes.h"
#include "mcp/tool.h"
#include "mcp/tool_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

using namespace sage::mcp;

namespace {

Tool makeNoopTool(std::string name) {
    return Tool{
        .name        = std::move(name),
        .description = "noop",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json&) -> ToolResult {
            return nlohmann::json::object();
        },
    };
}

}  // namespace

TEST_CASE("ToolRegistry registers and dispatches a tool", "[mcp][registry]") {
    ToolRegistry reg;

    Tool ping{
        .name        = "ping",
        .description = "test ping",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json& p) -> ToolResult {
            nlohmann::json out = {{"pong", true}};
            if (p.is_object() && p.contains("echo")) out["echo"] = p["echo"];
            return out;
        },
    };

    REQUIRE(reg.registerTool(std::move(ping)).has_value());
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.has("ping"));

    auto outcome = reg.dispatch("ping", nlohmann::json{{"echo", "hi"}});
    REQUIRE(outcome.has_value());
    REQUIRE((*outcome)["pong"] == true);
    REQUIRE((*outcome)["echo"] == "hi");
}

TEST_CASE("ToolRegistry rejects duplicate registration", "[mcp][registry]") {
    ToolRegistry reg;
    REQUIRE(reg.registerTool(makeNoopTool("x")).has_value());

    auto err = reg.registerTool(makeNoopTool("x"));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error() == ToolRegistry::RegisterError::DuplicateName);
}

TEST_CASE("ToolRegistry rejects invalid name", "[mcp][registry]") {
    ToolRegistry reg;

    SECTION("empty name") {
        auto err = reg.registerTool(makeNoopTool(""));
        REQUIRE_FALSE(err.has_value());
        REQUIRE(err.error() == ToolRegistry::RegisterError::InvalidName);
    }
    SECTION("disallowed character") {
        auto err = reg.registerTool(makeNoopTool("bad name"));
        REQUIRE_FALSE(err.has_value());
        REQUIRE(err.error() == ToolRegistry::RegisterError::InvalidName);
    }
}

TEST_CASE("ToolRegistry::dispatch returns MethodNotFound for unknown tool", "[mcp][registry]") {
    ToolRegistry reg;
    auto outcome = reg.dispatch("unknown", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::MethodNotFound);
}

TEST_CASE("ToolRegistry::dispatch traps handler exceptions", "[mcp][registry]") {
    ToolRegistry reg;
    Tool throwing{
        .name        = "boom",
        .description = "throws",
        .inputSchema = nlohmann::json::object(),
        .handler     = [](const nlohmann::json&) -> ToolResult {
            throw std::runtime_error("kaboom");
        },
    };
    REQUIRE(reg.registerTool(std::move(throwing)).has_value());

    auto outcome = reg.dispatch("boom", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}

TEST_CASE("ToolRegistry::list snapshots all tools", "[mcp][registry]") {
    ToolRegistry reg;
    REQUIRE(reg.registerTool(makeNoopTool("a")).has_value());
    REQUIRE(reg.registerTool(makeNoopTool("b")).has_value());
    REQUIRE(reg.registerTool(makeNoopTool("c")).has_value());

    auto items = reg.list();
    REQUIRE(items.size() == 3);
}

TEST_CASE("ToolRegistry rejects local tool with no handler", "[mcp][registry]") {
    ToolRegistry reg;
    Tool broken{
        .name        = "broken",
        .description = "missing handler",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = false,
    };
    auto err = reg.registerTool(std::move(broken));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error() == ToolRegistry::RegisterError::MissingHandler);
}

TEST_CASE("ToolRegistry routes remote tool via dispatcher", "[mcp][registry]") {
    ToolRegistry reg;

    Tool remote{
        .name        = "editor.ping",
        .description = "remote",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());
    REQUIRE_FALSE(reg.hasRemoteDispatcher());

    bool called = false;
    reg.setRemoteDispatcher(
        [&called](std::string_view tool, const nlohmann::json& args) -> ToolResult {
            called = true;
            REQUIRE(tool == "editor.ping");
            return nlohmann::json{{"echoed", args}};
        });
    REQUIRE(reg.hasRemoteDispatcher());

    auto outcome = reg.dispatch("editor.ping", nlohmann::json{{"message", "hi"}});
    REQUIRE(called);
    REQUIRE(outcome.has_value());
    REQUIRE((*outcome)["echoed"]["message"] == "hi");
}

TEST_CASE("ToolRegistry remote without dispatcher yields InternalError",
          "[mcp][registry]") {
    ToolRegistry reg;
    Tool remote{
        .name        = "rt",
        .description = "remote",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());

    auto outcome = reg.dispatch("rt", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}

TEST_CASE("ToolRegistry remote dispatcher exception → InternalError",
          "[mcp][registry]") {
    ToolRegistry reg;
    Tool remote{
        .name        = "rt",
        .description = "throws",
        .inputSchema = nlohmann::json::object(),
        .handler     = nullptr,
        .remote      = true,
    };
    REQUIRE(reg.registerTool(std::move(remote)).has_value());
    reg.setRemoteDispatcher(
        [](std::string_view, const nlohmann::json&) -> ToolResult {
            throw std::runtime_error("dispatcher boom");
        });

    auto outcome = reg.dispatch("rt", nlohmann::json::object());
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code == ErrorCode::InternalError);
}
