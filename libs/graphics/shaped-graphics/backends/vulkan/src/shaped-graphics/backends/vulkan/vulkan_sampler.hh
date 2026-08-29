#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>

/// sg's sampler value type translated into Vulkan.
///
/// Unlike D3D12, Vulkan keeps the per-axis min/mag/mip filters independent of anisotropy rather than folding
/// anisotropy into an encoded filter, so the mapping is field for field.

/// A VkSampler for every distinct sampler state a context has seen.
///
/// dx12 needs no such thing: CreateSampler writes a descriptor straight into a heap and leaves no object behind, while
/// a Vulkan sampler descriptor names a VkSampler that has to outlive every group holding it.
/// Caching per context rather than owning per group is what keeps that lifetime trivial — a re-minted group reuses the
/// object instead of scheduling the old one for deletion.
///
/// Keyed by sg::impl::sampler_hash, so the cache and a layout's identity agree on which samplers are the same one.
/// Sampler states are few and heavily repeated, so this never grows large enough to want eviction.
class sg::backend::vulkan::vulkan_sampler_cache
{
public:
    explicit vulkan_sampler_cache(vulkan_context& ctx) : _ctx(ctx) {}
    ~vulkan_sampler_cache();

    /// The sampler object for `s`, created on first request.
    /// Returns null only when sampler creation fails, which is out-of-memory rather than a bad state.
    [[nodiscard]] VkSampler acquire(sg::sampler const& s);

    void shutdown();

private:
    vulkan_context& _ctx;
    cc::mutex<cc::map<cc::hash128, VkSampler>> _samplers;
};

namespace sg::backend::vulkan
{
/// The VkSamplerCreateInfo an sg sampler describes.
/// A comparison sampler sets compareEnable.
/// The others leave compareOp at NEVER, which Vulkan ignores.
[[nodiscard]] VkSamplerCreateInfo to_vk_sampler_info(sg::sampler const& s);

/// The Vulkan comparison function for an sg compare_op.
/// Shared with depth-stencil state.
[[nodiscard]] VkCompareOp to_vk_compare_op(sg::compare_op op);
} // namespace sg::backend::vulkan
