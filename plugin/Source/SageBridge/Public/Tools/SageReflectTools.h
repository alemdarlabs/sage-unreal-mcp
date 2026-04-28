#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.1 — UE reflection introspection tools.
 *
 * Tools registered:
 *   reflect_class(class_path)
 *     Full UClass dump: name, parent, module, flags, interfaces, all
 *     UProperties (type, category, access, replication, tooltip),
 *     all UFunctions (signature, access, pure, network role),
 *     immediate child classes.
 *
 *   reflect_struct(struct_path)
 *     UScriptStruct dump: name, module, all fields with full type info.
 *
 *   reflect_enum(enum_path)
 *     UEnum dump: name, module, all entries (name + value + display +
 *     tooltip).
 *
 *   list_classes(filter?, base_class?, include_native?, include_blueprint?)
 *     Walk TObjectIterator<UClass>; same SKEL_/REINST_ filter as the
 *     scan_asset_registry indexer.
 *
 *   list_structs(filter?)        — all UScriptStructs.
 *   list_enums(filter?)          — all UEnums.
 *
 *   find_implementers(interface_path)
 *     Classes that implement the given UInterface.
 *
 *   class_default_object(class_path)
 *     Returns the CDO's properties as JSON (read-only — set via
 *     bp.set_cdo_property in Phase 4.2).
 *
 * Class paths accept both engine form (/Script/Engine.Pawn) and
 * Blueprint generated-class form (/Game/.../BP_Foo.BP_Foo_C).
 */
SAGEBRIDGE_API void RegisterReflectTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
