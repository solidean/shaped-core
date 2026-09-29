#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sg::backend::dx12
{
/// Maps an sg::pixel_format to its DXGI format; `undefined` maps to DXGI_FORMAT_UNKNOWN.
/// Depth formats map to their depth DXGI type, which is the format of a depth-stencil view and of a copy.
[[nodiscard]] DXGI_FORMAT to_dxgi_format(sg::pixel_format format);

/// The format a texture of `format` is created in.
/// **A depth texture a shader samples is created typeless**, since D3D12 takes no shader view of a depth format.
/// Every other texture is created in `to_dxgi_format`'s format.
[[nodiscard]] DXGI_FORMAT to_dxgi_resource_format(sg::pixel_format format, bool is_sampled);

/// The format a shader view of `format` reads through: a depth format's depth as the matching color format.
[[nodiscard]] DXGI_FORMAT to_dxgi_shader_view_format(sg::pixel_format format);
} // namespace sg::backend::dx12
