#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_state.hh>

namespace sg::backend::dx12
{
DXGI_FORMAT to_dxgi_vertex_format(sg::vertex_attribute_format f)
{
    switch (f)
    {
    case sg::vertex_attribute_format::f32:
        return DXGI_FORMAT_R32_FLOAT;
    case sg::vertex_attribute_format::vec2f:
        return DXGI_FORMAT_R32G32_FLOAT;
    case sg::vertex_attribute_format::vec3f:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case sg::vertex_attribute_format::vec4f:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case sg::vertex_attribute_format::i32:
        return DXGI_FORMAT_R32_SINT;
    case sg::vertex_attribute_format::vec2i:
        return DXGI_FORMAT_R32G32_SINT;
    case sg::vertex_attribute_format::vec3i:
        return DXGI_FORMAT_R32G32B32_SINT;
    case sg::vertex_attribute_format::vec4i:
        return DXGI_FORMAT_R32G32B32A32_SINT;
    case sg::vertex_attribute_format::u32:
        return DXGI_FORMAT_R32_UINT;
    case sg::vertex_attribute_format::vec2u:
        return DXGI_FORMAT_R32G32_UINT;
    case sg::vertex_attribute_format::vec3u:
        return DXGI_FORMAT_R32G32B32_UINT;
    case sg::vertex_attribute_format::vec4u:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    case sg::vertex_attribute_format::rgba8_unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case sg::vertex_attribute_format::rgba8_uint:
        return DXGI_FORMAT_R8G8B8A8_UINT;
    }
    CC_UNREACHABLE("unhandled vertex_attribute_format");
}

D3D12_FILL_MODE to_d3d12_fill_mode(sg::fill_mode m)
{
    return m == sg::fill_mode::wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

D3D12_CULL_MODE to_d3d12_cull_mode(sg::cull_mode m)
{
    switch (m)
    {
    case sg::cull_mode::none:
        return D3D12_CULL_MODE_NONE;
    case sg::cull_mode::front:
        return D3D12_CULL_MODE_FRONT;
    case sg::cull_mode::back:
        return D3D12_CULL_MODE_BACK;
    }
    CC_UNREACHABLE("unhandled cull_mode");
}

D3D12_BLEND to_d3d12_blend(sg::blend_factor f)
{
    switch (f)
    {
    case sg::blend_factor::zero:
        return D3D12_BLEND_ZERO;
    case sg::blend_factor::one:
        return D3D12_BLEND_ONE;
    case sg::blend_factor::src_color:
        return D3D12_BLEND_SRC_COLOR;
    case sg::blend_factor::one_minus_src_color:
        return D3D12_BLEND_INV_SRC_COLOR;
    case sg::blend_factor::dst_color:
        return D3D12_BLEND_DEST_COLOR;
    case sg::blend_factor::one_minus_dst_color:
        return D3D12_BLEND_INV_DEST_COLOR;
    case sg::blend_factor::src_alpha:
        return D3D12_BLEND_SRC_ALPHA;
    case sg::blend_factor::one_minus_src_alpha:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case sg::blend_factor::dst_alpha:
        return D3D12_BLEND_DEST_ALPHA;
    case sg::blend_factor::one_minus_dst_alpha:
        return D3D12_BLEND_INV_DEST_ALPHA;
    }
    CC_UNREACHABLE("unhandled blend_factor");
}

D3D12_BLEND_OP to_d3d12_blend_op(sg::blend_op op)
{
    switch (op)
    {
    case sg::blend_op::add:
        return D3D12_BLEND_OP_ADD;
    case sg::blend_op::subtract:
        return D3D12_BLEND_OP_SUBTRACT;
    case sg::blend_op::reverse_subtract:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case sg::blend_op::min:
        return D3D12_BLEND_OP_MIN;
    case sg::blend_op::max:
        return D3D12_BLEND_OP_MAX;
    }
    CC_UNREACHABLE("unhandled blend_op");
}

UINT8 to_d3d12_color_write_mask(sg::color_write_mask m)
{
    UINT8 out = 0;
    if (sg::has_flag(m, sg::color_write_mask::r))
        out |= D3D12_COLOR_WRITE_ENABLE_RED;
    if (sg::has_flag(m, sg::color_write_mask::g))
        out |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    if (sg::has_flag(m, sg::color_write_mask::b))
        out |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (sg::has_flag(m, sg::color_write_mask::a))
        out |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    return out;
}

D3D12_STENCIL_OP to_d3d12_stencil_op(sg::stencil_op op)
{
    switch (op)
    {
    case sg::stencil_op::keep:
        return D3D12_STENCIL_OP_KEEP;
    case sg::stencil_op::zero:
        return D3D12_STENCIL_OP_ZERO;
    case sg::stencil_op::replace:
        return D3D12_STENCIL_OP_REPLACE;
    case sg::stencil_op::increment_clamp:
        return D3D12_STENCIL_OP_INCR_SAT;
    case sg::stencil_op::decrement_clamp:
        return D3D12_STENCIL_OP_DECR_SAT;
    case sg::stencil_op::invert:
        return D3D12_STENCIL_OP_INVERT;
    case sg::stencil_op::increment_wrap:
        return D3D12_STENCIL_OP_INCR;
    case sg::stencil_op::decrement_wrap:
        return D3D12_STENCIL_OP_DECR;
    }
    CC_UNREACHABLE("unhandled stencil_op");
}

// Depth/stencil comparison — mirrors dx12_sampler.cc's to_comparison (kept local: a small switch is
// cheaper to duplicate than to hoist a shared helper across the two files).
D3D12_COMPARISON_FUNC to_d3d12_comparison(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case sg::compare_op::less:
        return D3D12_COMPARISON_FUNC_LESS;
    case sg::compare_op::equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case sg::compare_op::less_equal:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case sg::compare_op::greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case sg::compare_op::not_equal:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case sg::compare_op::greater_equal:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case sg::compare_op::always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    CC_UNREACHABLE("unhandled compare_op");
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(sg::primitive_topology_type t)
{
    switch (t)
    {
    case sg::primitive_topology_type::point:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case sg::primitive_topology_type::line:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case sg::primitive_topology_type::triangle:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case sg::primitive_topology_type::patch:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    }
    CC_UNREACHABLE("unhandled primitive_topology_type");
}

D3D12_PRIMITIVE_TOPOLOGY to_d3d12_topology(sg::primitive_topology t)
{
    switch (t)
    {
    case sg::primitive_topology::point_list:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case sg::primitive_topology::line_list:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case sg::primitive_topology::line_strip:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case sg::primitive_topology::triangle_list:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case sg::primitive_topology::triangle_strip:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case sg::primitive_topology::patch_list:
        // The concrete IA topology also encodes the control-point count — go through to_d3d12_patch_topology.
        CC_UNREACHABLE("patch_list needs a control-point count; use to_d3d12_patch_topology");
    }
    CC_UNREACHABLE("unhandled primitive_topology");
}

D3D12_PRIMITIVE_TOPOLOGY to_d3d12_patch_topology(int control_points)
{
    CC_ASSERT(control_points >= 1 && control_points <= 32, "patch control-point count must be 1..32");
    // The 32 N_CONTROL_POINT_PATCHLIST values are contiguous from the 1-control-point one.
    return D3D12_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + (control_points - 1));
}
} // namespace sg::backend::dx12
