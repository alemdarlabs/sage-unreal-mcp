#pragma once

namespace sage::mcp {
class ToolRegistry;
}

namespace sage::tools {

// Register all Phase 4 remote tool schemas (animation, gameplay, niagara,
// pcg, landscape, foliage, gas, networking, audio, level extensions, mat
// extensions, editor extensions, project extensions, widget extensions,
// bp extensions, asset extensions, seq extensions, reflection extensions).
//
// All registered tools are remote=true, handler=nullptr — they are dispatched
// through the WebSocket bridge to the plugin's SageBridgeSubsystem.
void registerPhase4Schemas(mcp::ToolRegistry& registry);

}  // namespace sage::tools
