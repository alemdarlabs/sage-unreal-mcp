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

}  // namespace

std::expected<void, ToolRegistry::RegisterError> ToolRegistry::registerTool(Tool tool) {
    if (!isValidToolName(tool.name)) {
        return std::unexpected(RegisterError::InvalidName);
    }
    if (tools_.contains(tool.name)) {
        return std::unexpected(RegisterError::DuplicateName);
    }
    spdlog::debug("Registering tool: {}", tool.name);
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
                                   const nlohmann::json& params) const {
    const Tool* tool = find(name);
    if (tool == nullptr) {
        return std::unexpected(ErrorObject::fromCode(
            ErrorCode::MethodNotFound, std::string{"unknown tool: "} + std::string{name}));
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

}  // namespace sage::mcp
