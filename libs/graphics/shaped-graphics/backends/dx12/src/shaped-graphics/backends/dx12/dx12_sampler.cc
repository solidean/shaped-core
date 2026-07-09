#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/sampler.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] D3D12_FILTER_TYPE filter_type(sg::sampler_filter f)
{
    return f == sg::sampler_filter::linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
}

// The D3D12_FILTER encodes min/mag/mip filter, the anisotropy flag, and the reduction (standard vs
// comparison). Anisotropy overrides the per-axis min/mag/mip choice on D3D12.
[[nodiscard]] D3D12_FILTER to_filter(sg::sampler const& s)
{
    D3D12_FILTER_REDUCTION_TYPE const reduction
        = s.compare.has_value() ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (s.max_anisotropy > 1)
        return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
    return D3D12_ENCODE_BASIC_FILTER(filter_type(s.min_filter), filter_type(s.mag_filter), filter_type(s.mip_filter),
                                     reduction);
}

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE to_address(sg::sampler_address_mode m)
{
    switch (m)
    {
    case sg::sampler_address_mode::repeat:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case sg::sampler_address_mode::mirror_repeat:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case sg::sampler_address_mode::clamp_edge:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case sg::sampler_address_mode::clamp_border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case sg::sampler_address_mode::mirror_clamp_edge:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    }
    CC_UNREACHABLE("unhandled sampler address mode");
}

// D3D12 always wants a valid ComparisonFunc; a non-comparison sampler ignores it, so NEVER is the filler.
[[nodiscard]] D3D12_COMPARISON_FUNC to_comparison(sg::compare_op op)
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

[[nodiscard]] UINT clamp_anisotropy(sg::u32 a)
{
    return UINT(a < 1 ? 1 : (a > 16 ? 16 : a)); // D3D12 caps MaxAnisotropy at 16
}

[[nodiscard]] D3D12_STATIC_BORDER_COLOR to_static_border(sg::sampler_border_color c)
{
    switch (c)
    {
    case sg::sampler_border_color::transparent_black:
        return D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    case sg::sampler_border_color::opaque_black:
        return D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    case sg::sampler_border_color::opaque_white:
        return D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    }
    CC_UNREACHABLE("unhandled sampler border color");
}

void fill_border(float (&out)[4], sg::sampler_border_color c)
{
    float const a = c == sg::sampler_border_color::transparent_black ? 0.0f : 1.0f;
    float const rgb = c == sg::sampler_border_color::opaque_white ? 1.0f : 0.0f;
    out[0] = out[1] = out[2] = rgb;
    out[3] = a;
}
} // namespace

D3D12_SAMPLER_DESC to_d3d12_sampler_desc(sg::sampler const& s)
{
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = to_filter(s);
    desc.AddressU = to_address(s.address_u);
    desc.AddressV = to_address(s.address_v);
    desc.AddressW = to_address(s.address_w);
    desc.MipLODBias = s.mip_lod_bias;
    desc.MaxAnisotropy = clamp_anisotropy(s.max_anisotropy);
    desc.ComparisonFunc = to_comparison(s.compare.value_or(sg::compare_op::never));
    fill_border(desc.BorderColor, s.border_color);
    desc.MinLOD = s.min_lod;
    desc.MaxLOD = s.max_lod;
    return desc;
}

D3D12_STATIC_SAMPLER_DESC to_d3d12_static_sampler_desc(sg::sampler const& s,
                                                       UINT shader_register,
                                                       UINT register_space,
                                                       D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_STATIC_SAMPLER_DESC desc = {};
    desc.Filter = to_filter(s);
    desc.AddressU = to_address(s.address_u);
    desc.AddressV = to_address(s.address_v);
    desc.AddressW = to_address(s.address_w);
    desc.MipLODBias = s.mip_lod_bias;
    desc.MaxAnisotropy = clamp_anisotropy(s.max_anisotropy);
    desc.ComparisonFunc = to_comparison(s.compare.value_or(sg::compare_op::never));
    desc.BorderColor = to_static_border(s.border_color);
    desc.MinLOD = s.min_lod;
    desc.MaxLOD = s.max_lod;
    desc.ShaderRegister = shader_register;
    desc.RegisterSpace = register_space;
    desc.ShaderVisibility = visibility;
    return desc;
}
} // namespace sg::backend::dx12
