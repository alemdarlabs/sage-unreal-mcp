#include "mcp/server.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace sage::mcp {

// MCP protocol revision we report in the initialize response.
// Reference: https://modelcontextprotocol.io
constexpr const char* PROTOCOL_VERSION = "2025-03-26";

MCPServer::MCPServer(ServerInfo info, std::shared_ptr<ToolRegistry> registry)
    : info_(std::move(info)), registry_(std::move(registry)) {
    if (registry_ == nullptr) {
        throw std::invalid_argument("MCPServer: ToolRegistry must not be null");
    }
}

std::optional<Response> MCPServer::handle(const Request& request) {
    spdlog::debug("MCP handle: method='{}' notification={}",
                  request.method, request.isNotification());

    // Notifications: side-effect only, no response per JSON-RPC 2.0.
    if (request.isNotification()) {
        if (request.method == "notifications/initialized") {
            initialized_ = true;
            spdlog::info("Client signaled initialized");
        }
        return std::nullopt;
    }

    Id id = *request.id;
    const std::string& method = request.method;

    if (method == "initialize") return onInitialize(std::move(id), request.params);
    if (method == "ping")       return onPing(std::move(id));
    if (method == "tools/list") return onToolsList(std::move(id));
    if (method == "tools/call") return onToolsCall(std::move(id), request.params);

    return Response::failure(std::move(id),
        ErrorObject::fromCode(ErrorCode::MethodNotFound,
                              std::string{"unknown method: "} + method));
}

std::optional<nlohmann::json> MCPServer::handleRaw(const nlohmann::json& payload) {
    Request req;
    try {
        req = Request::fromJson(payload);
    } catch (const std::exception& ex) {
        ErrorObject err = ErrorObject::fromCode(ErrorCode::InvalidRequest, ex.what());
        return Response::failure(nullptr, std::move(err)).toJson();
    }
    auto resp = handle(req);
    if (!resp) return std::nullopt;
    return resp->toJson();
}

Response MCPServer::onInitialize(Id id, const nlohmann::json& /*params*/) {
    nlohmann::json result = {
        {"protocolVersion", PROTOCOL_VERSION},
        {"capabilities", {
            {"tools", { {"listChanged", false} }},
        }},
        {"serverInfo", {
            {"name",    info_.name},
            {"version", info_.version},
        }},
    };
    return Response::success(std::move(id), std::move(result));
}

Response MCPServer::onPing(Id id) {
    return Response::success(std::move(id), nlohmann::json::object());
}

Response MCPServer::onToolsList(Id id) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tool : registry_->list()) {
        arr.push_back({
            {"name",        tool.name},
            {"description", tool.description},
            {"inputSchema", tool.inputSchema},
        });
    }
    nlohmann::json result = {{"tools", std::move(arr)}};
    return Response::success(std::move(id), std::move(result));
}

Response MCPServer::onToolsCall(Id id, const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return Response::failure(std::move(id),
            ErrorObject::fromCode(ErrorCode::InvalidParams,
                                  "missing or invalid 'name' field"));
    }
    const auto toolName  = params["name"].get<std::string>();
    nlohmann::json args  = params.value("arguments", nlohmann::json::object());

    auto outcome = registry_->dispatch(toolName, args);
    if (!outcome.has_value()) {
        return Response::failure(std::move(id), outcome.error());
    }

    // MCP `tools/call` shape: content array (text fallback) + structuredContent.
    nlohmann::json content = nlohmann::json::array();
    content.push_back({
        {"type", "text"},
        {"text", outcome->dump()},
    });
    nlohmann::json wrapped = {
        {"content",           std::move(content)},
        {"isError",           false},
        {"structuredContent", *outcome},
    };
    return Response::success(std::move(id), std::move(wrapped));
}

}  // namespace sage::mcp
