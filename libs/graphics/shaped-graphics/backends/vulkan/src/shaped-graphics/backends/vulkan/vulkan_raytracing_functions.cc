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

bool vulkan_raytracing_functions::load_micromaps(VkDevice device)
{
    auto const fetch = [device](char const* name) { return vkGetDeviceProcAddr(device, name); };

    create_micromap = PFN_vkCreateMicromapEXT(fetch("vkCreateMicromapEXT"));
    destroy_micromap = PFN_vkDestroyMicromapEXT(fetch("vkDestroyMicromapEXT"));
    get_micromap_build_sizes = PFN_vkGetMicromapBuildSizesEXT(fetch("vkGetMicromapBuildSizesEXT"));
    cmd_build_micromaps = PFN_vkCmdBuildMicromapsEXT(fetch("vkCmdBuildMicromapsEXT"));

    return create_micromap != nullptr && destroy_micromap != nullptr && get_micromap_build_sizes != nullptr
        && cmd_build_micromaps != nullptr;
}

bool vulkan_raytracing_functions::load_partitioned(VkDevice device)
{
    auto const fetch = [device](char const* name) { return vkGetDeviceProcAddr(device, name); };

    get_partitioned_build_sizes = PFN_vkGetPartitionedAccelerationStructuresBuildSizesNV(
        fetch("vkGetPartitionedAccelerationStructuresBuildSizesNV"));
    cmd_build_partitioned
        = PFN_vkCmdBuildPartitionedAccelerationStructuresNV(fetch("vkCmdBuildPartitionedAccelerationStructuresNV"));

    return get_partitioned_build_sizes != nullptr && cmd_build_partitioned != nullptr;
}

bool vulkan_raytracing_functions::load_clusters(VkDevice device)
{
    auto const fetch = [device](char const* name) { return vkGetDeviceProcAddr(device, name); };

    get_cluster_build_sizes
        = PFN_vkGetClusterAccelerationStructureBuildSizesNV(fetch("vkGetClusterAccelerationStructureBuildSizesNV"));
    cmd_build_clusters = PFN_vkCmdBuildClusterAccelerationStructureIndirectNV(fetch("vkCmdBuildClusterAccelerationStruc"
                                                                                    "tureIndirectNV"));

    return get_cluster_build_sizes != nullptr && cmd_build_clusters != nullptr;
}
} // namespace sg::backend::vulkan
