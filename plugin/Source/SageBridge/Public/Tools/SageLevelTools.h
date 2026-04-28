#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers level-domain tool handlers (level.get_outliner, level.spawn_light,
 * level.set_world_settings, etc.) into the supplied dispatch table.
 * Handlers marshal to GameThread internally; safe to invoke from WebSocket
 * worker threads.
 */
SAGEBRIDGE_API void RegisterLevelTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
