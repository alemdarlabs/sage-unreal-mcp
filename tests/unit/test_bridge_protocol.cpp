#include "bridge/protocol.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace sage::bridge;

TEST_CASE("parseType maps known strings", "[bridge][protocol]") {
    REQUIRE(parseType("hello")         == MessageType::Hello);
    REQUIRE(parseType("heartbeat")     == MessageType::Heartbeat);
    REQUIRE(parseType("tool_result")   == MessageType::ToolResult);
    REQUIRE(parseType("event")         == MessageType::Event);
    REQUIRE(parseType("welcome")       == MessageType::Welcome);
    REQUIRE(parseType("heartbeat_ack") == MessageType::HeartbeatAck);
    REQUIRE(parseType("tool_call")     == MessageType::ToolCall);
    REQUIRE(parseType("error")         == MessageType::Error);
}

TEST_CASE("parseType returns Unknown for garbage", "[bridge][protocol]") {
    REQUIRE(parseType("")        == MessageType::Unknown);
    REQUIRE(parseType("garbage") == MessageType::Unknown);
    REQUIRE(parseType("HELLO")   == MessageType::Unknown);  // case-sensitive
}

TEST_CASE("parseHello accepts well-formed envelope", "[bridge][protocol]") {
    const nlohmann::json env = {
        {"type",    "hello"},
        {"version", "0.1.0"},
        {"plugin_version", "0.1.5"},
        {"slot_id", "abc123"},
        {"editor", {
            {"id",    "MyProject@x"},
            {"label", "host"},
        }},
    };
    auto r = parseHello(env);
    REQUIRE(r.has_value());
    REQUIRE(r->version == "0.1.0");
    REQUIRE(r->plugin_version == "0.1.5");
    REQUIRE(r->slot_id == "abc123");
    REQUIRE(r->editor["id"] == "MyProject@x");
    REQUIRE(r->editor["label"] == "host");
    REQUIRE_FALSE(r->asset_registry_hash.has_value());
}

TEST_CASE("parseHello captures asset_registry_hash", "[bridge][protocol]") {
    const nlohmann::json env = {
        {"type",    "hello"},
        {"version", "0.1.0"},
        {"slot_id", "abc"},
        {"editor",  nlohmann::json::object()},
        {"asset_registry_hash", "sha256:dead..."},
    };
    auto r = parseHello(env);
    REQUIRE(r.has_value());
    REQUIRE(r->asset_registry_hash.has_value());
    REQUIRE(*r->asset_registry_hash == "sha256:dead...");
}

TEST_CASE("parseHello rejects malformed envelopes", "[bridge][protocol]") {
    SECTION("missing slot_id") {
        const nlohmann::json env = {{"type", "hello"}, {"version", "0.1.0"}};
        REQUIRE_FALSE(parseHello(env).has_value());
    }
    SECTION("missing version") {
        const nlohmann::json env = {{"type", "hello"}, {"slot_id", "x"}};
        REQUIRE_FALSE(parseHello(env).has_value());
    }
    SECTION("non-object envelope") {
        REQUIRE_FALSE(parseHello(nlohmann::json::array()).has_value());
    }
    SECTION("non-string slot_id") {
        const nlohmann::json env = {{"type", "hello"}, {"version", "0.1.0"}, {"slot_id", 42}};
        REQUIRE_FALSE(parseHello(env).has_value());
    }
}

TEST_CASE("parseHeartbeat extracts ts", "[bridge][protocol]") {
    SECTION("integer ts") {
        const nlohmann::json env = {{"type", "heartbeat"}, {"ts", 1714234567}};
        auto r = parseHeartbeat(env);
        REQUIRE(r.has_value());
        REQUIRE(r->ts == 1714234567);
    }
    SECTION("missing ts defaults to 0") {
        const nlohmann::json env = {{"type", "heartbeat"}};
        auto r = parseHeartbeat(env);
        REQUIRE(r.has_value());
        REQUIRE(r->ts == 0);
    }
}

TEST_CASE("parseToolResult validates tx_id", "[bridge][protocol]") {
    SECTION("happy path with result") {
        const nlohmann::json env = {
            {"type", "tool_result"},
            {"tx_id", "tx-1"},
            {"success", true},
            {"result", {{"actor_id", "BP_Pawn_1"}}},
            {"after_hash", "sha256:..."},
        };
        auto r = parseToolResult(env);
        REQUIRE(r.has_value());
        REQUIRE(r->tx_id == "tx-1");
        REQUIRE(r->success);
        REQUIRE(r->result.has_value());
        REQUIRE((*r->result)["actor_id"] == "BP_Pawn_1");
        REQUIRE(r->after_hash.has_value());
    }
    SECTION("error path") {
        const nlohmann::json env = {
            {"type", "tool_result"},
            {"tx_id", "tx-2"},
            {"success", false},
            {"error", {{"code", -32602}, {"message", "bad arg"}}},
        };
        auto r = parseToolResult(env);
        REQUIRE(r.has_value());
        REQUIRE_FALSE(r->success);
        REQUIRE(r->error.has_value());
        REQUIRE((*r->error)["code"] == -32602);
    }
    SECTION("missing tx_id") {
        const nlohmann::json env = {{"type", "tool_result"}, {"success", true}};
        REQUIRE_FALSE(parseToolResult(env).has_value());
    }
}

TEST_CASE("Message factories produce well-formed JSON", "[bridge][protocol]") {
    SECTION("welcomeMessage") {
        const auto j = welcomeMessage("sess-1", "0.1.0");
        REQUIRE(j["type"]           == "welcome");
        REQUIRE(j["session_id"]     == "sess-1");
        REQUIRE(j["server_version"] == "0.1.0");
    }
    SECTION("heartbeatAckMessage") {
        const auto j = heartbeatAckMessage(42);
        REQUIRE(j["type"] == "heartbeat_ack");
        REQUIRE(j["ts"]   == 42);
    }
    SECTION("toolCallMessage carries args verbatim") {
        const auto j = toolCallMessage("tx-9", "spawn_actor", {{"class", "Pawn"}});
        REQUIRE(j["type"]            == "tool_call");
        REQUIRE(j["tx_id"]           == "tx-9");
        REQUIRE(j["tool"]            == "spawn_actor");
        REQUIRE(j["args"]["class"]   == "Pawn");
    }
    SECTION("errorMessage") {
        const auto j = errorMessage(-32600, "invalid request");
        REQUIRE(j["type"]    == "error");
        REQUIRE(j["code"]    == -32600);
        REQUIRE(j["message"] == "invalid request");
    }
}
