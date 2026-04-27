#pragma once

#include "mcp/tool.h"

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sage::mcp {

// Thread-safety: registration during init, dispatch during request handling.
// V1 is read-mostly; if registration becomes dynamic post-init we will revisit
// with an immutable-snapshot or RCU pattern.
class ToolRegistry {
public:
    enum class RegisterError {
        DuplicateName,
        InvalidName,
    };

    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;
    ToolRegistry(ToolRegistry&&) noexcept = default;
    ToolRegistry& operator=(ToolRegistry&&) noexcept = default;
    ~ToolRegistry() = default;

    [[nodiscard]] std::expected<void, RegisterError> registerTool(Tool tool);

    [[nodiscard]] bool has(std::string_view name) const noexcept;
    [[nodiscard]] const Tool* find(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<Tool> list() const;
    [[nodiscard]] std::size_t size() const noexcept { return tools_.size(); }

    // `tools/call` semantics. Returns ErrorObject if tool absent or handler throws.
    [[nodiscard]] ToolResult dispatch(std::string_view name,
                                      const nlohmann::json& params) const;

private:
    // Heterogeneous lookup so string_view queries don't allocate a temporary string.
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
        std::size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string>{}(s);
        }
    };
    struct StringEq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };

    std::unordered_map<std::string, Tool, StringHash, StringEq> tools_;
};

}  // namespace sage::mcp
