#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers material-domain tool handlers (modify_material_parameter) into
 * the supplied dispatch table. Backed by UMaterialEditingLibrary; targets
 * UMaterialInstanceConstant assets in the content browser.
 */
SAGEBRIDGE_API void RegisterMaterialTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
