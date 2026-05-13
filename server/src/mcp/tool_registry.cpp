#include "mcp/tool_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace sage::mcp {

namespace {

bool isValidToolName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) return false;
    return std::ranges::all_of(name, [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (std::isalnum(u) != 0) || c == '_' || c == '/' || c == '.' || c == '-';
    });
}

void ensureObjectSchema(nlohmann::json& schema) {
    if (!schema.is_object()) {
        schema = {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        };
        return;
    }

    if (!schema.contains("type")) {
        schema["type"] = "object";
    }
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        schema["properties"] = nlohmann::json::object();
    }
}

void ensureRemoteAsyncSchema(Tool& tool) {
    if (!tool.remote) return;

    ensureObjectSchema(tool.inputSchema);
    auto& props = tool.inputSchema["properties"];
    if (!props.contains("async")) {
        props["async"] = {
            {"type", "boolean"},
            {"description",
             "When true, start this remote editor tool as a detached Sage job "
             "and return {job_id,state} immediately. Poll with jobs.get, "
             "jobs.wait, or jobs.logs. The plugin receives the same args with "
             "async removed."},
        };
    }
    if (!props.contains("job_timeout_seconds")) {
        props["job_timeout_seconds"] = {
            {"type", "integer"},
            {"minimum", 1},
            {"maximum", 86400},
            {"description",
             "Detached job editor-dispatch timeout. Default 3600 seconds."},
        };
    }
}

}  // namespace

std::expected<void, ToolRegistry::RegisterError> ToolRegistry::registerTool(Tool tool) {
    if (!isValidToolName(tool.name)) {
        return std::unexpected(RegisterError::InvalidName);
    }
    if (!tool.remote && !tool.handler) {
        return std::unexpected(RegisterError::MissingHandler);
    }
    if (tools_.contains(tool.name)) {
        return std::unexpected(RegisterError::DuplicateName);
    }
    spdlog::debug("Registering tool: {} (remote={})",
                  tool.name, tool.remote ? "yes" : "no");
    ensureRemoteAsyncSchema(tool);
    tools_.emplace(tool.name, std::move(tool));
    return {};
}

bool ToolRegistry::has(std::string_view name) const noexcept {
    return tools_.find(name) != tools_.end();
}

const Tool* ToolRegistry::find(std::string_view name) const noexcept {
    const auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<Tool> ToolRegistry::list() const {
    std::vector<Tool> out;
    out.reserve(tools_.size());
    for (const auto& [_, tool] : tools_) {
        out.push_back(tool);
    }
    return out;
}

ToolResult ToolRegistry::dispatch(std::string_view name,
                                   const nlohmann::json& params,
                                   std::string_view targetEditor) const {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        return std::unexpected(ErrorObject::fromCode(
            ErrorCode::MethodNotFound, std::string{"unknown tool: "} + std::string{name}));
    }

    if (tool->remote) {
        if (!remoteDispatcher_) {
            return std::unexpected(ErrorObject::fromCode(
                ErrorCode::InternalError, "remote dispatcher not configured"));
        }
        try {
            return remoteDispatcher_(name, params, targetEditor);
        } catch (const std::exception& ex) {
            spdlog::error("Remote dispatcher threw for '{}': {}", name, ex.what());
            return std::unexpected(ErrorObject::fromCode(
                ErrorCode::InternalError,
                std::string{"remote dispatch exception: "} + ex.what()));
        }
    }

    if (!tool->handler) {
        return std::unexpected(ErrorObject::fromCode(
            ErrorCode::InternalError, "local tool has no handler"));
    }
    try {
        return tool->handler(params);
    } catch (const std::exception& ex) {
        spdlog::error("Tool '{}' threw exception: {}", name, ex.what());
        return std::unexpected(ErrorObject::fromCode(
            ErrorCode::InternalError, std::string{"tool exception: "} + ex.what()));
    } catch (...) {
        spdlog::error("Tool '{}' threw unknown exception", name);
        return std::unexpected(
            ErrorObject::fromCode(ErrorCode::InternalError, "tool unknown exception"));
    }
}

void ToolRegistry::setRemoteDispatcher(RemoteDispatcher fn) {
    remoteDispatcher_ = std::move(fn);
}

bool ToolRegistry::hasRemoteDispatcher() const noexcept {
    return static_cast<bool>(remoteDispatcher_);
}

}  // namespace sage::mcp
