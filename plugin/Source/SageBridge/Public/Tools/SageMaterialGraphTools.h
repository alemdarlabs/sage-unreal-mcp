#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.3 — Material graph authoring (read + write).
 *
 * Existing Phase 1 `modify_material_parameter` lives in SageMaterialTools
 * and continues to set scalar/vector parameters on UMaterialInstanceConstant.
 * This file adds the wider material-graph surface.
 *
 * Read:
 *   mat.read(path)               — domain, blend mode, shading model, counts
 *   mat.list_parameters(path)    — scalar/vector/texture/static-switch params
 *   mat.list_expressions(path)   — full graph node list
 *   mat.read_graph(path)         — nodes + connections + positions
 *   mat.get_shader_stats(path)   — texture samples, instructions, registers
 *
 * Write:
 *   mat.create(path, domain, blend_mode, shading_model)
 *   mat.create_instance(parent, dest)
 *   mat.add_expression(path, expr_class, position)
 *   mat.delete_expression(path, node_id)
 *   mat.connect_expressions(path, src_node, src_output, dst_node, dst_input)
 *   mat.disconnect(path, dst_node, dst_input)
 *   mat.connect_to_property(path, src_node, src_output, mat_prop)
 *   mat.set_expression_value(path, node_id, value)
 *   mat.connect_texture(path, expr_id, texture_path)
 *   mat.set_shading_model(path, model)
 *   mat.set_base_color(path, color)
 *
 * Compile + validate:
 *   mat.validate(path)           — compile shader, return stats + warnings
 */
SAGEBRIDGE_API void RegisterMaterialGraphTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
