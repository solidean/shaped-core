#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>

/// The VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline entry points, loaded once per device.
///
/// Same reason as the descriptor-buffer loader: an extension's commands are not exported by the loader's static
/// symbols, so they come through vkGetDeviceProcAddr.
/// Unlike that one these are optional — a device without ray tracing comes up fine and answers
/// `cmd.raytracing.is_supported()` false — so a failed load is a normal outcome rather than a driver bug.

struct sg::backend::vulkan::vulkan_raytracing_functions
{
    // VK_KHR_acceleration_structure
    PFN_vkCreateAccelerationStructureKHR create_acceleration_structure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_device_address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_acceleration_structures = nullptr;

    // VK_KHR_ray_tracing_pipeline
    PFN_vkCreateRayTracingPipelinesKHR create_raytracing_pipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_shader_group_handles = nullptr;
    PFN_vkCmdTraceRaysKHR cmd_trace_rays = nullptr;

    /// Fetches every entry point; returns false when any is missing.
    [[nodiscard]] bool load(VkDevice device);

    [[nodiscard]] bool is_loaded() const { return cmd_build_acceleration_structures != nullptr; }
};
