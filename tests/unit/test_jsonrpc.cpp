#include "mcp/error_codes.h"
#include "mcp/types.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using namespace sage::mcp;

TEST_CASE("Request::fromJson parses well-formed request", "[mcp][jsonrpc]") {
    const nlohmann::json env = {
        {"jsonrpc", "2.0"},
        {"method",  "ping"},
        {"id",      1},
        {"params",  {{"echo", "hi"}}},
    };
    auto req = Request::fromJson(env);

    REQUIRE(req.method == "ping");
    REQUIRE(req.id.has_value());
    REQUIRE(*req.id == 1);
    REQUIRE(req.params == nlohmann::json{{"echo", "hi"}});
    REQUIRE_FALSE(req.isNotification());
}

TEST_CASE("Request::fromJson detects notification (no id)", "[mcp][jsonrpc]") {
    const nlohmann::json env = {
        {"jsonrpc", "2.0"},
        {"method",  "notifications/initialized"},
    };
    auto req = Request::fromJson(env);
    REQUIRE(req.isNotification());
    REQUIRE(req.method == "notifications/initialized");
}

TEST_CASE("Request::fromJson defaults missing params to empty object", "[mcp][jsonrpc]") {
    const nlohmann::json env = {
        {"jsonrpc", "2.0"},
        {"method",  "tools/list"},
        {"id",      "abc"},
    };
    auto req = Request::fromJson(env);
    REQUIRE(req.params == nlohmann::json::object());
}

TEST_CASE("Request::fromJson rejects malformed envelopes", "[mcp][jsonrpc]") {
    SECTION("missing jsonrpc field") {
        const nlohmann::json env = {{"method", "ping"}, {"id", 1}};
        REQUIRE_THROWS_AS(Request::fromJson(env), std::runtime_error);
    }
    SECTION("wrong jsonrpc version") {
        const nlohmann::json env = {{"jsonrpc", "1.0"}, {"method", "ping"}, {"id", 1}};
        REQUIRE_THROWS_AS(Request::fromJson(env), std::runtime_error);
    }
    SECTION("missing method") {
        const nlohmann::json env = {{"jsonrpc", "2.0"}, {"id", 1}};
        REQUIRE_THROWS_AS(Request::fromJson(env), std::runtime_error);
    }
    SECTION("non-string method") {
        const nlohmann::json env = {{"jsonrpc", "2.0"}, {"method", 42}, {"id", 1}};
        REQUIRE_THROWS_AS(Request::fromJson(env), std::runtime_error);
    }
    SECTION("non-object envelope") {
        const nlohmann::json env = nlohmann::json::array();
        REQUIRE_THROWS_AS(Request::fromJson(env), std::runtime_error);
    }
}

TEST_CASE("Response::success serializes result", "[mcp][jsonrpc]") {
    const auto resp = Response::success(7, nlohmann::json{{"pong", true}});
    const auto j = resp.toJson();

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["id"] == 7);
    REQUIRE(j["result"]["pong"] == true);
    REQUIRE_FALSE(j.contains("error"));
}

TEST_CASE("Response::failure serializes error object", "[mcp][jsonrpc]") {
    const auto err  = ErrorObject::fromCode(ErrorCode::MethodNotFound, "ping");
    const auto resp = Response::failure(7, err);
    const auto j    = resp.toJson();

    REQUIRE(j["jsonrpc"] == "2.0");
    REQUIRE(j["id"] == 7);
    REQUIRE(j["error"]["code"] == -32601);
    REQUIRE(j["error"]["message"].get<std::string>().find("Method not found")
            != std::string::npos);
    REQUIRE_FALSE(j.contains("result"));
}

TEST_CASE("Response with null id is JSON-RPC compliant", "[mcp][jsonrpc]") {
    const auto resp = Response::failure(nullptr, ErrorObject::fromCode(ErrorCode::ParseError));
    const auto j    = resp.toJson();
    REQUIRE(j["id"].is_null());
}

TEST_CASE("ErrorObject::fromCode formats default message", "[mcp][jsonrpc]") {
    const auto err = ErrorObject::fromCode(ErrorCode::InvalidParams);
    REQUIRE(err.code == ErrorCode::InvalidParams);
    REQUIRE(err.message == "Invalid params");
}

TEST_CASE("ErrorObject::fromCode appends detail", "[mcp][jsonrpc]") {
    const auto err = ErrorObject::fromCode(ErrorCode::SlotNotFound, "abc-123");
    REQUIRE(err.code == ErrorCode::SlotNotFound);
    REQUIRE(err.message == "Slot not found: abc-123");
}
