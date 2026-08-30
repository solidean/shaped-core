#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_state.hh>

namespace sg::backend::vulkan
{
VkPrimitiveTopology to_vk_topology(sg::primitive_topology t)
{
    switch (t)
    {
    case sg::primitive_topology::point_list:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case sg::primitive_topology::line_list:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case sg::primitive_topology::line_strip:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case sg::primitive_topology::triangle_list:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case sg::primitive_topology::triangle_strip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case sg::primitive_topology::patch_list:
        return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    CC_UNREACHABLE("unhandled primitive_topology");
}

VkPolygonMode to_vk_polygon_mode(sg::fill_mode m)
{
    switch (m)
    {
    case sg::fill_mode::solid:
        return VK_POLYGON_MODE_FILL;
    case sg::fill_mode::wireframe:
        return VK_POLYGON_MODE_LINE;
    }
    CC_UNREACHABLE("unhandled fill_mode");
}

VkCullModeFlags to_vk_cull_mode(sg::cull_mode m)
{
    switch (m)
    {
    case sg::cull_mode::none:
        return VK_CULL_MODE_NONE;
    case sg::cull_mode::front:
        return VK_CULL_MODE_FRONT_BIT;
    case sg::cull_mode::back:
        return VK_CULL_MODE_BACK_BIT;
    }
    CC_UNREACHABLE("unhandled cull_mode");
}

VkFrontFace to_vk_front_face(sg::front_face f)
{
    switch (f)
    {
    case sg::front_face::counter_clockwise:
        return VK_FRONT_FACE_COUNTER_CLOCKWISE;
    case sg::front_face::clockwise:
        return VK_FRONT_FACE_CLOCKWISE;
    }
    CC_UNREACHABLE("unhandled front_face");
}

VkBlendFactor to_vk_blend_factor(sg::blend_factor f)
{
    switch (f)
    {
    case sg::blend_factor::zero:
        return VK_BLEND_FACTOR_ZERO;
    case sg::blend_factor::one:
        return VK_BLEND_FACTOR_ONE;
    case sg::blend_factor::src_color:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case sg::blend_factor::one_minus_src_color:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case sg::blend_factor::dst_color:
        return VK_BLEND_FACTOR_DST_COLOR;
    case sg::blend_factor::one_minus_dst_color:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case sg::blend_factor::src_alpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case sg::blend_factor::one_minus_src_alpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case sg::blend_factor::dst_alpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case sg::blend_factor::one_minus_dst_alpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    CC_UNREACHABLE("unhandled blend_factor");
}

VkBlendOp to_vk_blend_op(sg::blend_op op)
{
    switch (op)
    {
    case sg::blend_op::add:
        return VK_BLEND_OP_ADD;
    case sg::blend_op::subtract:
        return VK_BLEND_OP_SUBTRACT;
    case sg::blend_op::reverse_subtract:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case sg::blend_op::min:
        return VK_BLEND_OP_MIN;
    case sg::blend_op::max:
        return VK_BLEND_OP_MAX;
    }
    CC_UNREACHABLE("unhandled blend_op");
}

VkColorComponentFlags to_vk_color_write_mask(sg::color_write_mask mask)
{
    VkColorComponentFlags flags = 0;
    if (mask.has(sg::color_channel::r))
        flags |= VK_COLOR_COMPONENT_R_BIT;
    if (mask.has(sg::color_channel::g))
        flags |= VK_COLOR_COMPONENT_G_BIT;
    if (mask.has(sg::color_channel::b))
        flags |= VK_COLOR_COMPONENT_B_BIT;
    if (mask.has(sg::color_channel::a))
        flags |= VK_COLOR_COMPONENT_A_BIT;
    return flags;
}

VkStencilOp to_vk_stencil_op(sg::stencil_op op)
{
    switch (op)
    {
    case sg::stencil_op::keep:
        return VK_STENCIL_OP_KEEP;
    case sg::stencil_op::zero:
        return VK_STENCIL_OP_ZERO;
    case sg::stencil_op::replace:
        return VK_STENCIL_OP_REPLACE;
    case sg::stencil_op::increment_clamp:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case sg::stencil_op::decrement_clamp:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case sg::stencil_op::invert:
        return VK_STENCIL_OP_INVERT;
    case sg::stencil_op::increment_wrap:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case sg::stencil_op::decrement_wrap:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    CC_UNREACHABLE("unhandled stencil_op");
}

VkFormat to_vk_vertex_format(sg::vertex_attribute_format f)
{
    switch (f)
    {
    case sg::vertex_attribute_format::f32:
        return VK_FORMAT_R32_SFLOAT;
    case sg::vertex_attribute_format::vec2f:
        return VK_FORMAT_R32G32_SFLOAT;
    case sg::vertex_attribute_format::vec3f:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case sg::vertex_attribute_format::vec4f:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case sg::vertex_attribute_format::i32:
        return VK_FORMAT_R32_SINT;
    case sg::vertex_attribute_format::vec2i:
        return VK_FORMAT_R32G32_SINT;
    case sg::vertex_attribute_format::vec3i:
        return VK_FORMAT_R32G32B32_SINT;
    case sg::vertex_attribute_format::vec4i:
        return VK_FORMAT_R32G32B32A32_SINT;
    case sg::vertex_attribute_format::u32:
        return VK_FORMAT_R32_UINT;
    case sg::vertex_attribute_format::vec2u:
        return VK_FORMAT_R32G32_UINT;
    case sg::vertex_attribute_format::vec3u:
        return VK_FORMAT_R32G32B32_UINT;
    case sg::vertex_attribute_format::vec4u:
        return VK_FORMAT_R32G32B32A32_UINT;
    case sg::vertex_attribute_format::rgba8_unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case sg::vertex_attribute_format::rgba8_uint:
        return VK_FORMAT_R8G8B8A8_UINT;
    }
    CC_UNREACHABLE("unhandled vertex_attribute_format");
}
} // namespace sg::backend::vulkan
