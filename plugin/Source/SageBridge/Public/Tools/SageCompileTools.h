#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers compile / Live Coding handlers (get_live_coding_status,
 * compile_and_reload). Live Coding is Windows-only in UE 5.7; on macOS/Linux
 * `compile_and_reload` returns -32007 LiveCodingUnavailable, but
 * `get_live_coding_status` always responds (with `available=false` on
 * non-Windows hosts). Full restart orchestration (save→shutdown→UBT→relaunch)
 * lands in Phase 2 per ADR-009.
 */
SAGEBRIDGE_API void RegisterCompileTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
