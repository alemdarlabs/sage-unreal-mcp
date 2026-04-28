#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers Phase 2 knowledge-layer feeders.
 *
 * `_scan_asset_registry` (internal — invoked only by sage-server's
 * `index_slot` MCP tool, not exposed in the registry):
 *   args: none
 *   result: {
 *     assets: [{path: "/Game/...", kind: "Blueprint"}, ...],
 *     scan_ms: number,
 *     total: number
 *   }
 *
 * Walks the AssetRegistry on the game thread (cheap; cached in-memory).
 * UE 5.7's AssetRegistry waits for any in-flight scan via
 * WaitForCompletion before returning. If editor is still loading content,
 * scan completes once the registry has fully populated.
 */
SAGEBRIDGE_API void RegisterIndexTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
