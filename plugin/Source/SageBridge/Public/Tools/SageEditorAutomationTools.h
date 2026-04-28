#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.6 — Editor automation tools.
 *
 * Bridges the gap between Sage's existing editor-state tools (get_world,
 * get_pie_state, ...) and the broader editor automation surface ue-mcp
 * exposes.
 *
 *   editor.console_command(cmd, allow_unsafe?)
 *     Execute a UE console command. Default: gated; only the read-only /
 *     navigation set is whitelisted. allow_unsafe=true bypasses the
 *     whitelist (use with caution — UE console can crash the editor).
 *
 *   editor.take_screenshot(path?)
 *     Capture the active viewport to a PNG. Returns the saved file path.
 *
 *   editor.get_engine_version() / editor.get_project_version()
 *     Read engine + project metadata.
 *
 *   editor.read_log(category?, level?, since_ms?)
 *     Recent log lines from the editor's log file (post-handshake slice).
 *
 *   editor.get_log_file_path()
 *     Return the absolute path of the current editor log file. The agent
 *     can tail it directly via filesystem if it needs more than a slice.
 */
SAGEBRIDGE_API void RegisterEditorAutomationTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
