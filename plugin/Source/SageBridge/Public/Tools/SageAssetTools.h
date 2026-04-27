#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers asset-domain tool handlers (modify_asset_property, rename_asset,
 * move_asset, duplicate_asset, delete_asset) into the supplied dispatch table.
 * Handlers marshal to GameThread internally; backed by UEditorAssetSubsystem.
 */
SAGEBRIDGE_API void RegisterAssetTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
