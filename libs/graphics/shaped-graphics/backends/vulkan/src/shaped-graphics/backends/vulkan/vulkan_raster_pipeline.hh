#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

/// vulkan raster pipeline: a graphics VkPipeline built with **dynamic rendering**, so it carries its target formats
/// rather than a VkRenderPass.
///
/// `sg::raster_pipeline_description` already has the shape dynamic rendering wants — the color formats, the
/// depth-stencil format and the sample count are on the pipeline, not on a pass object — so there is nothing to
/// invent here and no render-pass or framebuffer cache to keep.
///
/// Viewport, scissor, stencil reference and blend constants are `VkDynamicState`, matching the sg surface where each
/// is a command rather than pipeline state.
///
/// The cached-blob handling is the compute pipeline's; see vulkan_compute_pipeline for why the header is validated
/// rather than the creation result.
class sg::backend::vulkan::vulkan_raster_pipeline final : public sg::raster_pipeline
{
public:
    explicit vulkan_raster_pipeline(vulkan_context& ctx) : _ctx(ctx) {}

    [[nodiscard]] static cc::result<vulkan_raster_pipeline_handle> create(vulkan_context& ctx,
                                                                          sg::raster_pipeline_description const& desc);

    ~vulkan_raster_pipeline() override;

    /// Destroys the backend objects now; see sg::raster_pipeline::release_backend_objects.
    void release_backend_objects() override;

    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override;

    vulkan_context& _ctx;
    vulkan_pipeline_layout_handle layout;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineCache _cache = VK_NULL_HANDLE;
};
