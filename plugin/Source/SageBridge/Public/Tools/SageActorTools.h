#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers actor-domain tool handlers (spawn_actor, ...) into the supplied
 * dispatch table. Handlers marshal to GameThread internally; safe to invoke
 * from WebSocket worker threads.
 */
SAGEBRIDGE_API void RegisterActorTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
