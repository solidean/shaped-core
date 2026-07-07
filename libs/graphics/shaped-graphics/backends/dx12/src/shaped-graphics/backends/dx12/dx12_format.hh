#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/pixel_format.hh>

namespace sg::backend::dx12
{
/// Maps an sg::pixel_format to its DXGI resource format. `undefined` maps to DXGI_FORMAT_UNKNOWN. Depth
/// formats map to their depth DXGI type (typeless resource + view formats arrive with texture views).
[[nodiscard]] DXGI_FORMAT to_dxgi_format(sg::pixel_format format);
} // namespace sg::backend::dx12
