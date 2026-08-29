#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>

/// vulkan ray-tracing pipeline: a VkPipeline of ray-tracing shader groups.
///
/// The mapping is close to DXR's, because sg's description already names the same things: a raygen / miss / callable
/// shader is a *general* group of one stage, and a hit_shader is a triangles or procedural group of up to three.
/// Whether the hit group has an intersection stage is what makes it procedural, exactly as sg documents.
///
/// A shader group is addressed by its index, and `vkGetRayTracingShaderGroupHandlesKHR` returns one opaque handle per
/// group — the direct analogue of DXR's 32-byte shader identifier, except that its size is a device property rather
/// than a constant.
/// The handles are read once here and kept, so building a shader table is a copy rather than a device call.
class sg::backend::vulkan::vulkan_raytracing_pipeline final : public sg::raytracing_pipeline
{
public:
    explicit vulkan_raytracing_pipeline(vulkan_context& ctx) : _ctx(ctx) {}

    [[nodiscard]] static cc::result<vulkan_raytracing_pipeline_handle> create(
        vulkan_context& ctx,
        sg::raytracing_pipeline_description const& desc);

    ~vulkan_raytracing_pipeline() override;

    /// Ray-tracing pipelines carry no serialized blob here: VkPipelineCache covers the ordinary tiers, and a
    /// state-object blob has no Vulkan counterpart.
    /// dx12 answers the same way, and the answer a caller needs is the same — nothing here is worth persisting.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

    /// The group handle for one registered shader, by the handle sg gave it.
    /// Each is `shaderGroupHandleSize` bytes, which is what a shader-table record holds.
    [[nodiscard]] cc::span<byte const> raygen_handle(sg::raygen_shader_handle h) const;
    [[nodiscard]] cc::span<byte const> miss_handle(sg::miss_shader_handle h) const;
    [[nodiscard]] cc::span<byte const> hit_handle(sg::hit_shader_handle h) const;
    [[nodiscard]] cc::span<byte const> callable_handle(sg::callable_shader_handle h) const;

    vulkan_context& _ctx;
    vulkan_pipeline_layout_handle layout;
    VkPipeline _pipeline = VK_NULL_HANDLE;

    /// Every group's opaque handle, concatenated in group order, and where each category's groups start.
    /// One allocation rather than four, since the device hands them back as one block.
    cc::vector<byte> _group_handles;
    isize _handle_size = 0;
    int _raygen_base = 0;
    int _miss_base = 0;
    int _hit_base = 0;
    int _callable_base = 0;
    int _group_count = 0;
};
