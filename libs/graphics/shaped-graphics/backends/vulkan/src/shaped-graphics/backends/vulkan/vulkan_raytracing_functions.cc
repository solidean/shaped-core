#include <shaped-graphics/backends/vulkan/vulkan_raytracing_functions.hh>

namespace sg::backend::vulkan
{
bool vulkan_raytracing_functions::load(VkDevice device)
{
    auto const fetch = [device](char const* name) { return vkGetDeviceProcAddr(device, name); };

    create_acceleration_structure = PFN_vkCreateAccelerationStructureKHR(fetch("vkCreateAccelerationStructureKHR"));
    destroy_acceleration_structure = PFN_vkDestroyAccelerationStructureKHR(fetch("vkDestroyAccelerationStructureKHR"));
    get_build_sizes = PFN_vkGetAccelerationStructureBuildSizesKHR(fetch("vkGetAccelerationStructureBuildSizesKHR"));
    get_device_address
        = PFN_vkGetAccelerationStructureDeviceAddressKHR(fetch("vkGetAccelerationStructureDeviceAddressKHR"));
    cmd_build_acceleration_structures
        = PFN_vkCmdBuildAccelerationStructuresKHR(fetch("vkCmdBuildAccelerationStructuresKHR"));

    create_raytracing_pipelines = PFN_vkCreateRayTracingPipelinesKHR(fetch("vkCreateRayTracingPipelinesKHR"));
    get_shader_group_handles = PFN_vkGetRayTracingShaderGroupHandlesKHR(fetch("vkGetRayTracingShaderGroupHandlesKHR"));
    cmd_trace_rays = PFN_vkCmdTraceRaysKHR(fetch("vkCmdTraceRaysKHR"));

    return create_acceleration_structure != nullptr && destroy_acceleration_structure != nullptr
        && get_build_sizes != nullptr && get_device_address != nullptr && cmd_build_acceleration_structures != nullptr
        && create_raytracing_pipelines != nullptr && get_shader_group_handles != nullptr && cmd_trace_rays != nullptr;
}
} // namespace sg::backend::vulkan
