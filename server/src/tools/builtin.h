#pragma once

namespace sage::mcp {
class ToolRegistry;
}

namespace sage::tools {

// Register Phase 1 baseline built-in tools (currently: ping).
// Subsequent milestones will add domain tools (actor/component/asset).
void registerBuiltins(mcp::ToolRegistry& registry);

}  // namespace sage::tools
