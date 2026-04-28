#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.7 — Project introspection (filesystem-only, no UObject reads).
 *
 * The "agent understands the project" surface — read .uproject metadata,
 * enumerate native modules from Source/, parse C++ headers for UCLASS /
 * USTRUCT / UENUM declarations, and read source files. All read-only;
 * useful for an agent that wants to plan changes before mutating the
 * editor.
 *
 *   project.get_info()
 *     Read .uproject + return engine_association, project name, path,
 *     declared modules + plugins enabled flag.
 *
 *   project.list_modules()
 *     Walk Source/<Module>/<Module>.Build.cs and collect each module's
 *     name + path. Equivalent to "what's in my Source folder".
 *
 *   project.read_cpp_header(path)
 *     Read a .h file (relative to project root or absolute), regex-scan
 *     for UCLASS / USTRUCT / UENUM declarations + #include directives.
 *     Returns {classes[], structs[], enums[], includes[], line_count}.
 *
 *   project.read_cpp_source(path, max_bytes?)
 *     Read a .cpp / .h / .inl source file as a string. Capped at
 *     max_bytes (default 64KB, max 512KB) so an over-eager agent
 *     doesn't blow the response payload.
 */
SAGEBRIDGE_API void RegisterProjectTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
