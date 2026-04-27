#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers `compare_and_set_property` — optimistic locking for primitive
 * UProperty mutations across actor / component / asset targets. Returns
 * -32003 VersionConflict if the current value does not match `expected`.
 */
SAGEBRIDGE_API void RegisterCompareSetTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
