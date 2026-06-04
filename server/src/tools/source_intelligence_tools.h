#pragma once

namespace sage::mcp {
class ToolRegistry;
}

namespace sage::tools {

// Register local, server-side source intelligence tools. These tools inspect
// source files, docs, ADRs, and git history directly; they do not dispatch to
// the Unreal editor bridge.
void registerSourceIntelligenceTools(mcp::ToolRegistry& registry);

}  // namespace sage::tools
