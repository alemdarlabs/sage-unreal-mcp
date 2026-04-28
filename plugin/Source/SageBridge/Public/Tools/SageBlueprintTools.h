#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.2 — Blueprint authoring (full read + write).
 *
 * Read:
 *   bp.read(path)                      — top-level summary
 *   bp.list_variables(path)            — all class variables
 *   bp.list_functions(path)            — all functions / events
 *   bp.list_local_variables(path, fn)  — function-local vars
 *   bp.read_function_graph(path, fn)   — node list + connections
 *   bp.read_event_graph(path)          — same for the event graph
 *   bp.read_construction_script(path)  — same for the CS
 *   bp.get_execution_flow(path, fn)    — BFS exec-pin walk
 *   bp.read_components(path)           — SCS hierarchy + defaults
 *   bp.list_node_types(filter?)        — palette query
 *   bp.search_nodes(path, kw)          — text search across graphs
 *   bp.get_dependencies(path)          — referenced classes / functions
 *
 * Write — graph:
 *   bp.add_node, bp.delete_node, bp.connect_pins, bp.disconnect_pins,
 *   bp.set_node_property, bp.move_node,
 *   bp.export_nodes_t3d, bp.import_nodes_t3d
 *
 * Write — variables:
 *   bp.add_variable, bp.delete_variable, bp.rename_variable,
 *   bp.set_variable_default, bp.set_variable_properties,
 *   bp.add_local_variable, bp.delete_local_variable
 *
 * Write — functions:
 *   bp.add_function, bp.delete_function, bp.rename_function,
 *   bp.set_function_properties, bp.add_function_input,
 *   bp.add_function_output, bp.remove_function_param
 *
 * Write — class shape:
 *   bp.reparent, bp.set_cdo_property,
 *   bp.add_interface, bp.remove_interface
 *
 * Write — components (SCS):
 *   bp.add_bp_component, bp.remove_bp_component,
 *   bp.reparent_component, bp.set_bp_component_property
 *
 * Compile + validate:
 *   bp.compile, bp.validate, bp.run_construction_script
 */
SAGEBRIDGE_API void RegisterBlueprintTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
