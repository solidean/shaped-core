#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/fwd.hh>

/// vulkan compute pipeline: a VkPipeline built from a SPIR-V module against a pipeline layout.
///
/// **The cached blob is a VkPipelineCache**, one per pipeline, which is how sg's per-pipeline blob maps onto an API
/// whose cache is normally shared.
/// Serializing it is `vkGetPipelineCacheData`; feeding one back is `vkCreatePipelineCache`'s initial data.
///
/// `used_cached_pipeline()` needs care, because Vulkan **silently ignores** a blob it cannot use where D3D12 fails
/// creation outright.
/// So the answer comes from validating the blob's header against the device — the same check the driver makes, and the
/// reason the header's layout is specified at all — rather than from whether creation succeeded.
class sg::backend::vulkan::vulkan_compute_pipeline final : public sg::compute_pipeline
{
public:
    vulkan_compute_pipeline(vulkan_context& ctx, sg::compute_dimensions workgroup_size)
      : sg::compute_pipeline(workgroup_size), _ctx(ctx)
    {
    }

    [[nodiscard]] static cc::result<vulkan_compute_pipeline_handle> create(vulkan_context& ctx,
                                                                           vulkan_pipeline_layout_handle layout,
                                                                           sg::compiled_shader const& shader,
                                                                           cc::span<byte const> cached_pipeline);

    ~vulkan_compute_pipeline() override;

    /// Destroys the backend objects now; see sg::compute_pipeline::release_backend_objects.
    void release_backend_objects() override;

    /// The pipeline cache's serialized form; empty when it holds nothing or serialization fails.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override;

    vulkan_context& _ctx;
    vulkan_pipeline_layout_handle layout;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineCache _cache = VK_NULL_HANDLE;
};

namespace sg::backend::vulkan
{
/// Whether `blob` is a pipeline-cache blob this device will actually use.
///
/// A driver checks the header's version, vendor, device and cache UUID and starts from an empty cache when any of them
/// disagrees — without telling the caller.
/// Running the same check is what turns "we handed one over" into "it was used", which is what sg asks for.
[[nodiscard]] bool is_usable_pipeline_cache_blob(vulkan_context const& ctx, cc::span<byte const> blob);
} // namespace sg::backend::vulkan
