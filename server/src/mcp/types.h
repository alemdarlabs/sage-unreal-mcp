#pragma once

#include "mcp/error_codes.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

// JSON-RPC 2.0 envelope primitives. Header-only; small-and-stable enough that
// the inline footprint cost is negligible compared to the include hygiene win.

namespace sage::mcp {

inline constexpr const char* JSONRPC_VERSION = "2.0";

// JSON-RPC id: number, string, or null. nlohmann::json captures all three
// without a hand-rolled variant.
using Id = nlohmann::json;

struct ErrorObject {
    ErrorCode code{ErrorCode::InternalError};
    std::string message;
    std::optional<nlohmann::json> data;

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json j = {
            {"code",    static_cast<int>(code)},
            {"message", message},
        };
        if (data) j["data"] = *data;
        return j;
    }

    [[nodiscard]] static ErrorObject fromCode(ErrorCode c) {
        return ErrorObject{c, describe(c), std::nullopt};
    }

    [[nodiscard]] static ErrorObject fromCode(ErrorCode c, std::string detail) {
        std::string msg = describe(c);
        if (!detail.empty()) {
            msg += ": ";
            msg += std::move(detail);
        }
        return ErrorObject{c, std::move(msg), std::nullopt};
    }
};

// Either a request (has id) or a notification (no id). MCP-side we currently
// only consume client-issued envelopes, so this serves both roles.
struct Request {
    std::string method;
    nlohmann::json params{nlohmann::json::object()};
    std::optional<Id> id;  // none ⇒ notification

    [[nodiscard]] bool isNotification() const noexcept { return !id.has_value(); }

    [[nodiscard]] static Request fromJson(const nlohmann::json& j) {
        if (!j.is_object()) throw std::runtime_error("envelope must be a JSON object");
        if (!j.contains("jsonrpc") || j["jsonrpc"] != JSONRPC_VERSION)
            throw std::runtime_error("invalid or missing 'jsonrpc' version");
        if (!j.contains("method") || !j["method"].is_string())
            throw std::runtime_error("missing or non-string 'method' field");

        Request r;
        r.method = j["method"].get<std::string>();
        r.params = j.value("params", nlohmann::json::object());
        if (j.contains("id")) r.id = j["id"];
        return r;
    }

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json j = {
            {"jsonrpc", JSONRPC_VERSION},
            {"method",  method},
            {"params",  params},
        };
        if (id) j["id"] = *id;
        return j;
    }
};

struct Response {
    Id id;
    std::variant<nlohmann::json, ErrorObject> payload;

    [[nodiscard]] bool isError() const noexcept {
        return std::holds_alternative<ErrorObject>(payload);
    }

    [[nodiscard]] nlohmann::json toJson() const {
        nlohmann::json j = {{"jsonrpc", JSONRPC_VERSION}, {"id", id}};
        if (std::holds_alternative<ErrorObject>(payload)) {
            j["error"] = std::get<ErrorObject>(payload).toJson();
        } else {
            j["result"] = std::get<nlohmann::json>(payload);
        }
        return j;
    }

    [[nodiscard]] static Response success(Id id, nlohmann::json result) {
        return Response{std::move(id), std::move(result)};
    }

    [[nodiscard]] static Response failure(Id id, ErrorObject err) {
        return Response{std::move(id), std::move(err)};
    }
};

}  // namespace sage::mcp
