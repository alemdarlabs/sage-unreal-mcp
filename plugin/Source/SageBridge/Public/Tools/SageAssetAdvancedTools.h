#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.5 — Asset advanced surface.
 *
 * Existing Phase 1 SageAssetTools (rename/move/duplicate/delete/save/...)
 * stays as-is. This file adds the missing advanced surface from the
 * ue-mcp audit.
 *
 * Read:
 *   asset.get_mesh_bounds(path)
 *   asset.get_mesh_collision(path)
 *   asset.list_redirectors(folder?)
 *   asset.diagnose_registry()
 *
 * Write:
 *   asset.bulk_rename([{src, dst}])      — atomic multi-rename in one tx
 *   asset.move_folder(src, dst)          — folder move with redirector fixup
 *   asset.fixup_redirectors([paths])     — resolve and remove redirectors
 */
SAGEBRIDGE_API void RegisterAssetAdvancedTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
