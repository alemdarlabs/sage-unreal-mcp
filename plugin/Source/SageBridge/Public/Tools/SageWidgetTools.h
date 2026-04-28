#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers UMG / Widget Blueprint authoring handlers (Phase 4.11 round 1):
 * widget.create — create UWidgetBlueprint via UWidgetBlueprintFactory
 * widget.list   — enumerate widget blueprints under a directory
 * widget.read   — read root widget tree + named slots
 *
 * All authoring goes through the GameThread (UE editor mutation rule).
 */
SAGEBRIDGE_API void RegisterWidgetTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
