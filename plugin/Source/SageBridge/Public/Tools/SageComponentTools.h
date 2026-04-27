#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers component-domain tool handlers (add_component, remove_component,
 * modify_component_property, attach, detach) into the supplied dispatch table.
 * Handlers marshal to GameThread internally.
 */
SAGEBRIDGE_API void RegisterComponentTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
