#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/blend_state.hh>
#include <shaped-graphics/depth_stencil_state.hh>
#include <shaped-graphics/primitive_topology.hh>
#include <shaped-graphics/rasterization_state.hh>
#include <shaped-graphics/sampler.hh> // compare_op
#include <shaped-graphics/vertex_input.hh>

/// Maps sg's raster fixed-function state vocabulary to the D3D12 enums a graphics PSO records. Bodies in
/// dx12_raster_state.cc.

namespace sg::backend::dx12
{
[[nodiscard]] DXGI_FORMAT to_dxgi_vertex_format(sg::vertex_attribute_format f);
[[nodiscard]] D3D12_FILL_MODE to_d3d12_fill_mode(sg::fill_mode m);
[[nodiscard]] D3D12_CULL_MODE to_d3d12_cull_mode(sg::cull_mode m);
[[nodiscard]] D3D12_BLEND to_d3d12_blend(sg::blend_factor f);
[[nodiscard]] D3D12_BLEND_OP to_d3d12_blend_op(sg::blend_op op);
[[nodiscard]] UINT8 to_d3d12_color_write_mask(sg::color_write_mask m);
[[nodiscard]] D3D12_STENCIL_OP to_d3d12_stencil_op(sg::stencil_op op);
[[nodiscard]] D3D12_COMPARISON_FUNC to_d3d12_comparison(sg::compare_op op);
[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(sg::primitive_topology_type t);
[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY to_d3d12_topology(sg::primitive_topology t);
/// IA topology for a `control_points`-per-patch patch list (tessellation). control_points must be 1..32.
[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY to_d3d12_patch_topology(int control_points);
} // namespace sg::backend::dx12
