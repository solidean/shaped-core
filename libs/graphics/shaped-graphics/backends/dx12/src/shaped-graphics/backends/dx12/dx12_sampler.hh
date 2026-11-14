#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Translates a backend-neutral sampler into the D3D12 descriptor a *dynamic* sampler is
/// created with (CreateSampler into the shader-visible sampler heap).
[[nodiscard]] D3D12_SAMPLER_DESC to_d3d12_sampler_desc(sg::sampler const& s);

/// Translates a sampler into a *static* sampler baked into a root signature, addressed at
/// (register = binding.index, space = binding.space) with the given shader visibility.
[[nodiscard]] D3D12_STATIC_SAMPLER_DESC to_d3d12_static_sampler_desc(sg::sampler const& s,
                                                                     UINT shader_register,
                                                                     UINT register_space,
                                                                     D3D12_SHADER_VISIBILITY visibility);

/// The descriptor a static sampler's state is as a dynamic sampler, for one bound through a table instead.
[[nodiscard]] D3D12_SAMPLER_DESC to_d3d12_sampler_desc(D3D12_STATIC_SAMPLER_DESC const& s);
} // namespace sg::backend::dx12

/// Every sampler state a context's root signatures bind through a table, each written once into the persistent region
/// of the shader-visible sampler heap and never freed.
///
/// **Why there are no static samplers on dx12.**
/// NVIDIA's driver restores a cached PSO with the static sampler of another root signature that differs from its own
/// in that sampler alone (docs/bugs-external/d3d12-cached-pso-static-sampler-mixup).
/// Bound through a table at the pipeline's bind, a sampler is no part of the root signature, so a clamping and a
/// repeating variant of one shader are one PSO, and its blob is safe to persist.
///
/// A slot is never freed, so nothing binds one past its life; a context meets a few dozen states, far below the heap.
/// Free-threaded: layouts are created on any thread.
struct sg::backend::dx12::dx12_sampler_interner
{
    /// The heap slot holding `desc`, written on first sight; null when the heap's persistent region is full.
    [[nodiscard]] cc::optional<int> intern(ID3D12Device* device, dx12_descriptor_heap& heap, D3D12_SAMPLER_DESC const& desc);

private:
    struct entry
    {
        D3D12_SAMPLER_DESC desc = {};
        int offset = 0;
    };
    cc::mutex<cc::vector<entry>> _entries;
};
