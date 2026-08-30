#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

/// vulkan ray-tracing shader table: a GPU buffer of shader-group handles in four sections, plus the address regions
/// vkCmdTraceRaysKHR reads.
///
/// `VkStridedDeviceAddressRegionKHR` is nearly D3D12's `D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE`, including the
/// quirk that the raygen region names exactly one record — Vulkan spells that as size == stride rather than as a
/// separate rangeless type.
///
/// Two alignments, both device properties rather than constants: a record aligns to `shaderGroupHandleAlignment` and
/// a section to `shaderGroupBaseAlignment`.
class sg::backend::vulkan::vulkan_raytracing_shader_table final : public sg::raytracing_shader_table
{
public:
    /// Builds the table: validates the handles against the pipeline, lays out the four sections, uploads the records,
    /// and captures the address regions.
    /// Requires at least one raygen record.
    [[nodiscard]] static cc::result<vulkan_raytracing_shader_table_handle> create(
        vulkan_context& ctx,
        sg::raytracing_shader_table_description const& desc);

    explicit vulkan_raytracing_shader_table(sg::raytracing_pipeline_handle pipeline)
      : sg::raytracing_shader_table(cc::move(pipeline))
    {
    }

    /// The single raygen record at `index`, as the one-record region vkCmdTraceRaysKHR takes.
    [[nodiscard]] VkStridedDeviceAddressRegionKHR raygen_record(sg::raygen_index index) const;

    vulkan_buffer_handle buffer; // backing buffer (kept alive; declared shader_read at dispatch)

    // Section regions; an empty section stays {} (address 0), which a trace treats as unused.
    VkStridedDeviceAddressRegionKHR raygen_table = {};
    VkStridedDeviceAddressRegionKHR miss_table = {};
    VkStridedDeviceAddressRegionKHR hit_table = {};
    VkStridedDeviceAddressRegionKHR callable_table = {};
};
