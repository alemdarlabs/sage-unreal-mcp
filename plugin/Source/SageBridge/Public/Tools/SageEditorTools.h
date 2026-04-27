#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers editor-state queries (get_world, get_pie_state, get_viewport_state)
 * and selection tools (get_selected_actors, select_actors, clear_selection).
 * State queries are read-only; selection tools mutate the editor selection set.
 */
SAGEBRIDGE_API void RegisterEditorTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
