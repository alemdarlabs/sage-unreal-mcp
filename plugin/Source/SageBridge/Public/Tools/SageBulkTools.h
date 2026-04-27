#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers `bulk_modify` — a multi-op tool that chains other registered
 * handlers either inside a single FScopedTransaction (atomic, default) or
 * each in its own transaction (atomic=false). Needs the dispatch table
 * itself (passed by reference) so it can re-enter sibling handlers.
 */
SAGEBRIDGE_API void RegisterBulkTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
