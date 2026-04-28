#pragma once

#include "bridge/bridge_server.h"
#include "mcp/tool.h"

#include <filesystem>
#include <string>

// Phase 3 — Milestone 1.6b: full editor-restart orchestrator.
//
// Wraps the save→shutdown→build→relaunch loop that the operator does by
// hand (the unreal-close + build-plugin.sh + unreal-open skill chain) into
// a single MCP tool. Critical because Mac has no Live Coding (UE 5.7 LC is
// Windows-only); the only way to land plugin changes there is a full
// restart.
//
// The handler is server-side LOCAL: it sequences other server primitives
// (bridge.dispatchTool to plugin save_assets), shells out to UBT, signals
// the editor PID, copies the dylib, relaunches, and waits for the new
// handshake on the same slot_id.
//
// Required environment:
//   SAGE_REPO_ROOT      — root of the sage-unreal-mcp checkout (defaults to cwd).
//                          scripts/build-plugin.sh + build/plugin/Binaries/<plat>/
//                          live under this path.
//   SAGE_UE_ROOT        — UE 5.7 install (passed through to build-plugin.sh).
//
// Args (JSON):
//   confirmed     bool      REQUIRED true. Hard guard against an LLM
//                            'oh I'll just restart' loop.
//   build_plugin  bool      default true — set false to relaunch the
//                            existing dylib.
//   save_dirty    bool      default true — save_assets before kill.
//   wait_handshake_sec int  default 90 — 0 returns immediately after
//                            relaunch.
//   slot_id       string    optional — restart this specific slot. Default:
//                            active session, then unique connected session.

namespace sage::tools {

struct RestartConfig {
    std::filesystem::path repoRoot;
    std::filesystem::path ueRoot;       // optional; passed via env to script
};

[[nodiscard]] mcp::Tool buildRestartEditorTool(bridge::BridgeServer& bridge,
                                                RestartConfig cfg);

}  // namespace sage::tools
