#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Translates a backend-neutral sampler_description into the D3D12 descriptor a *dynamic* sampler is
/// created with (CreateSampler into the shader-visible sampler heap).
[[nodiscard]] D3D12_SAMPLER_DESC to_d3d12_sampler_desc(sg::sampler_description const& s);

/// Translates a sampler_description into a *static* sampler baked into a root signature, addressed at
/// (register = binding.index, space = binding.set) with the given shader visibility.
[[nodiscard]] D3D12_STATIC_SAMPLER_DESC to_d3d12_static_sampler_desc(sg::sampler_description const& s,
                                                                     UINT shader_register,
                                                                     UINT register_space,
                                                                     D3D12_SHADER_VISIBILITY visibility);
} // namespace sg::backend::dx12
