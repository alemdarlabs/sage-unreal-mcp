#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers source control handlers (get_source_control_state,
 * checkout_files). Wraps ISourceControlModule provider with FCheckOut
 * operation. Phase 1 keeps it explicit (auto-checkout from mutation tools
 * lands as a follow-up wired into modify_asset_property + save_assets).
 */
SAGEBRIDGE_API void RegisterScmTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
