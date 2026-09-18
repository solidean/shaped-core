#include "metal_raster_state.hh"

namespace sg::backend::metal
{
MTL::PrimitiveTopologyClass topology_class_of(sg::primitive_topology topology)
{
    switch (topology)
    {
    case sg::primitive_topology::point_list:
        return MTL::PrimitiveTopologyClassPoint;
    case sg::primitive_topology::line_list:
    case sg::primitive_topology::line_strip:
        return MTL::PrimitiveTopologyClassLine;
    case sg::primitive_topology::triangle_list:
    case sg::primitive_topology::triangle_strip:
        return MTL::PrimitiveTopologyClassTriangle;
    case sg::primitive_topology::patch_list:
        // Tessellation is not a stage this backend reaches, and sg::feature::tessellation_shader reports false, so a
        // patch list cannot get this far through a pipeline build.
        return MTL::PrimitiveTopologyClassUnspecified;
    }
    return MTL::PrimitiveTopologyClassTriangle;
}

MTL::PrimitiveType primitive_type_of(sg::primitive_topology topology)
{
    switch (topology)
    {
    case sg::primitive_topology::point_list:
        return MTL::PrimitiveTypePoint;
    case sg::primitive_topology::line_list:
        return MTL::PrimitiveTypeLine;
    case sg::primitive_topology::line_strip:
        return MTL::PrimitiveTypeLineStrip;
    case sg::primitive_topology::triangle_list:
        return MTL::PrimitiveTypeTriangle;
    case sg::primitive_topology::triangle_strip:
        return MTL::PrimitiveTypeTriangleStrip;
    case sg::primitive_topology::patch_list:
        return MTL::PrimitiveTypeTriangle;
    }
    return MTL::PrimitiveTypeTriangle;
}

MTL::TriangleFillMode fill_mode_of(sg::fill_mode fill)
{
    return fill == sg::fill_mode::wireframe ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
}

MTL::CullMode cull_mode_of(sg::cull_mode cull)
{
    switch (cull)
    {
    case sg::cull_mode::none:
        return MTL::CullModeNone;
    case sg::cull_mode::front:
        return MTL::CullModeFront;
    case sg::cull_mode::back:
        return MTL::CullModeBack;
    }
    return MTL::CullModeNone;
}

MTL::Winding winding_of(sg::front_face front)
{
    return front == sg::front_face::clockwise ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
}

MTL::CompareFunction compare_of(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return MTL::CompareFunctionNever;
    case sg::compare_op::less:
        return MTL::CompareFunctionLess;
    case sg::compare_op::equal:
        return MTL::CompareFunctionEqual;
    case sg::compare_op::less_equal:
        return MTL::CompareFunctionLessEqual;
    case sg::compare_op::greater:
        return MTL::CompareFunctionGreater;
    case sg::compare_op::not_equal:
        return MTL::CompareFunctionNotEqual;
    case sg::compare_op::greater_equal:
        return MTL::CompareFunctionGreaterEqual;
    case sg::compare_op::always:
        return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionAlways;
}

MTL::StencilOperation stencil_op_of(sg::stencil_op op)
{
    switch (op)
    {
    case sg::stencil_op::keep:
        return MTL::StencilOperationKeep;
    case sg::stencil_op::zero:
        return MTL::StencilOperationZero;
    case sg::stencil_op::replace:
        return MTL::StencilOperationReplace;
    case sg::stencil_op::increment_clamp:
        return MTL::StencilOperationIncrementClamp;
    case sg::stencil_op::decrement_clamp:
        return MTL::StencilOperationDecrementClamp;
    case sg::stencil_op::invert:
        return MTL::StencilOperationInvert;
    case sg::stencil_op::increment_wrap:
        return MTL::StencilOperationIncrementWrap;
    case sg::stencil_op::decrement_wrap:
        return MTL::StencilOperationDecrementWrap;
    }
    return MTL::StencilOperationKeep;
}

MTL::BlendFactor blend_factor_of(sg::blend_factor factor)
{
    switch (factor)
    {
    case sg::blend_factor::zero:
        return MTL::BlendFactorZero;
    case sg::blend_factor::one:
        return MTL::BlendFactorOne;
    case sg::blend_factor::src_color:
        return MTL::BlendFactorSourceColor;
    case sg::blend_factor::one_minus_src_color:
        return MTL::BlendFactorOneMinusSourceColor;
    case sg::blend_factor::dst_color:
        return MTL::BlendFactorDestinationColor;
    case sg::blend_factor::one_minus_dst_color:
        return MTL::BlendFactorOneMinusDestinationColor;
    case sg::blend_factor::src_alpha:
        return MTL::BlendFactorSourceAlpha;
    case sg::blend_factor::one_minus_src_alpha:
        return MTL::BlendFactorOneMinusSourceAlpha;
    case sg::blend_factor::dst_alpha:
        return MTL::BlendFactorDestinationAlpha;
    case sg::blend_factor::one_minus_dst_alpha:
        return MTL::BlendFactorOneMinusDestinationAlpha;
    }
    return MTL::BlendFactorOne;
}

MTL::BlendOperation blend_op_of(sg::blend_op op)
{
    switch (op)
    {
    case sg::blend_op::add:
        return MTL::BlendOperationAdd;
    case sg::blend_op::subtract:
        return MTL::BlendOperationSubtract;
    case sg::blend_op::reverse_subtract:
        return MTL::BlendOperationReverseSubtract;
    case sg::blend_op::min:
        return MTL::BlendOperationMin;
    case sg::blend_op::max:
        return MTL::BlendOperationMax;
    }
    return MTL::BlendOperationAdd;
}

MTL::ColorWriteMask color_write_mask_of(sg::color_write_mask mask)
{
    MTL::ColorWriteMask out = MTL::ColorWriteMaskNone;
    if (mask.has(sg::color_channel::r))
        out |= MTL::ColorWriteMaskRed;
    if (mask.has(sg::color_channel::g))
        out |= MTL::ColorWriteMaskGreen;
    if (mask.has(sg::color_channel::b))
        out |= MTL::ColorWriteMaskBlue;
    if (mask.has(sg::color_channel::a))
        out |= MTL::ColorWriteMaskAlpha;
    return out;
}
} // namespace sg::backend::metal
