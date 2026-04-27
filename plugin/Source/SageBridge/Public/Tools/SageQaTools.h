#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers QA / Automation Framework handlers (list_tests, run_tests).
 * Wraps FAutomationTestFramework. Tests run async; run_tests is fire-and-
 * forget with the started list returned synchronously. Result polling is
 * deferred to Phase 2.
 */
SAGEBRIDGE_API void RegisterQaTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
