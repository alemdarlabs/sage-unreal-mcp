#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers multi-step transaction handlers (begin / commit / rollback). Wraps
 * UTransactor (GEditor->BeginTransaction / EndTransaction / CancelTransaction)
 * with a UUID-keyed registry so the agent can hold an open transaction across
 * multiple tool calls.
 *
 * Single-flat-level model: nested begin/begin without intervening
 * commit/rollback is permitted (UE supports it) but each pair must be
 * balanced; the most recently opened tx commits/rolls back via its own tx_id.
 */
SAGEBRIDGE_API void RegisterTransactionTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
