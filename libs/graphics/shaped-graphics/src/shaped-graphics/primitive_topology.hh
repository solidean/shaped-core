#pragma once

#include <shaped-graphics/fwd.hh>

/// The primitive kind a raster pipeline assembles vertices into. Baked into the pipeline (dynamic
/// per-draw topology is a future addition), so a pipeline is built for one topology.

namespace sg
{
/// How vertices are assembled into primitives. The concrete topology, not the coarse family: a backend
/// derives the PSO's topology *type* (point / line / triangle) from it via `topology_type`.
enum class primitive_topology
{
    point_list,     // DX12 POINTLIST     / Vk POINT_LIST
    line_list,      // DX12 LINELIST      / Vk LINE_LIST
    line_strip,     // DX12 LINESTRIP     / Vk LINE_STRIP
    triangle_list,  // DX12 TRIANGLELIST  / Vk TRIANGLE_LIST
    triangle_strip, // DX12 TRIANGLESTRIP / Vk TRIANGLE_STRIP
};

/// The coarse family a topology belongs to — the granularity a dx12 PSO records
/// (D3D12_PRIMITIVE_TOPOLOGY_TYPE), distinct from the concrete `primitive_topology` set on the IA.
enum class primitive_topology_type
{
    point,
    line,
    triangle,
};

/// The coarse family `t` assembles into.
[[nodiscard]] constexpr primitive_topology_type topology_type(primitive_topology t)
{
    switch (t)
    {
    case primitive_topology::point_list:
        return primitive_topology_type::point;
    case primitive_topology::line_list:
    case primitive_topology::line_strip:
        return primitive_topology_type::line;
    case primitive_topology::triangle_list:
    case primitive_topology::triangle_strip:
        return primitive_topology_type::triangle;
    }
    return primitive_topology_type::triangle;
}
} // namespace sg
