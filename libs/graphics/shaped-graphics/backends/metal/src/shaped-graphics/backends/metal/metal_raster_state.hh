#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/raster/depth_stencil_state.hh>
#include <shaped-graphics/raster/primitive_topology.hh>
#include <shaped-graphics/raster/rasterization_state.hh>
#include <shaped-graphics/raster/vertex_input.hh>

// Translating sg's raster state into Metal's.
//
// Device-free, like the barrier and format translations beside it, so its tests run on any machine rather than only
// where a Metal 4 GPU exists.

namespace sg::backend::metal
{
/// The primitive class a topology belongs to — what an MTL4 render pipeline is built with.
///
/// **Metal splits one question into two where sg and the other backends have one.**
/// A pipeline is built for a primitive *class* (point / line / triangle) and the draw names the exact *topology*, so a
/// strip and a list share a pipeline here and need separate ones on D3D12.
[[nodiscard]] MTL::PrimitiveTopologyClass topology_class_of(sg::primitive_topology topology);

/// The exact primitive type a draw is issued with.
[[nodiscard]] MTL::PrimitiveType primitive_type_of(sg::primitive_topology topology);

[[nodiscard]] MTL::TriangleFillMode fill_mode_of(sg::fill_mode fill);
[[nodiscard]] MTL::CullMode cull_mode_of(sg::cull_mode cull);
[[nodiscard]] MTL::Winding winding_of(sg::front_face front);

[[nodiscard]] MTL::CompareFunction compare_of(sg::compare_op op);
[[nodiscard]] MTL::StencilOperation stencil_op_of(sg::stencil_op op);

[[nodiscard]] MTL::BlendFactor blend_factor_of(sg::blend_factor factor);
[[nodiscard]] MTL::BlendOperation blend_op_of(sg::blend_op op);
[[nodiscard]] MTL::ColorWriteMask color_write_mask_of(sg::color_write_mask mask);

/// The MTLVertexFormat one vertex attribute decodes with.
///
/// sg's attribute set is the intersection every backend supports, so each enumerator has a Metal counterpart and the
/// mapping is total.
[[nodiscard]] MTL::VertexFormat vertex_format_of(sg::vertex_attribute_format format);

/// The MTLIndexType an indexed draw fetches with.
[[nodiscard]] MTL::IndexType index_type_of(sg::index_format format);

/// Bytes one index occupies, which is what turns a first-index into the byte offset `drawIndexedPrimitives` takes.
[[nodiscard]] isize index_size_of(sg::index_format format);
} // namespace sg::backend::metal
