#include "vulkan_driver_statistics.hh"

#include <shaped-graphics/backends/vulkan/vulkan_compute_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_pipeline.hh>

// VK_KHR_pipeline_executable_properties: a pipeline is one executable per compiled program, a stage or a variant of
// one, and each executable a list of named statistics the driver chose.

cc::vector<sg::driver_statistic> sg::backend::vulkan::driver_statistics_of(vulkan_context const& ctx, VkPipeline pipeline)
{
    auto out = cc::vector<sg::driver_statistic>();
    if (!ctx.captures_driver_statistics() || pipeline == VK_NULL_HANDLE)
        return out;
    auto const info = VkPipelineInfoKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    auto count = u32(0);
    if (ctx._get_pipeline_executable_properties(ctx._device, &info, &count, nullptr) != VK_SUCCESS)
        return out;
    auto executables = cc::vector<VkPipelineExecutablePropertiesKHR>::create_filled(
        count, VkPipelineExecutablePropertiesKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (ctx._get_pipeline_executable_properties(ctx._device, &info, &count, executables.data()) != VK_SUCCESS)
        return out;
    for (auto e = u32(0); e < count; ++e)
    {
        auto const which = VkPipelineExecutableInfoKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                                                       .pipeline = pipeline,
                                                       .executableIndex = e};
        auto statistics = u32(0);
        if (ctx._get_pipeline_executable_statistics(ctx._device, &which, &statistics, nullptr) != VK_SUCCESS)
            continue;
        auto found = cc::vector<VkPipelineExecutableStatisticKHR>::create_filled(
            statistics, VkPipelineExecutableStatisticKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        if (ctx._get_pipeline_executable_statistics(ctx._device, &which, &statistics, found.data()) != VK_SUCCESS)
            continue;
        for (auto const& s : found)
        {
            auto value = f64(0);
            switch (s.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                value = s.value.b32 == VK_TRUE ? 1.0 : 0.0;
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                value = f64(s.value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                value = f64(s.value.u64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                value = s.value.f64;
                break;
            default:
                break;
            }
            out.push_back({.executable = cc::string(executables[e].name),
                           .name = cc::string(s.name),
                           .description = cc::string(s.description),
                           .value = value});
        }
    }
    return out;
}

cc::vector<sg::driver_representation> sg::backend::vulkan::driver_representations_of(vulkan_context const& ctx,
                                                                                     VkPipeline pipeline)
{
    auto out = cc::vector<sg::driver_representation>();
    if (!ctx.captures_driver_representations() || pipeline == VK_NULL_HANDLE)
        return out;
    auto const info = VkPipelineInfoKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    auto count = u32(0);
    if (ctx._get_pipeline_executable_properties(ctx._device, &info, &count, nullptr) != VK_SUCCESS)
        return out;
    auto executables = cc::vector<VkPipelineExecutablePropertiesKHR>::create_filled(
        count, VkPipelineExecutablePropertiesKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (ctx._get_pipeline_executable_properties(ctx._device, &info, &count, executables.data()) != VK_SUCCESS)
        return out;
    for (auto e = u32(0); e < count; ++e)
    {
        auto const which = VkPipelineExecutableInfoKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                                                       .pipeline = pipeline,
                                                       .executableIndex = e};
        // first how many forms, then each one's size, then their data
        auto forms = u32(0);
        if (ctx._get_pipeline_executable_representations(ctx._device, &which, &forms, nullptr) != VK_SUCCESS)
            continue;
        auto found = cc::vector<VkPipelineExecutableInternalRepresentationKHR>::create_filled(
            forms, VkPipelineExecutableInternalRepresentationKHR{
                       .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        if (ctx._get_pipeline_executable_representations(ctx._device, &which, &forms, found.data()) != VK_SUCCESS)
            continue;
        auto storage = cc::vector<cc::vector<char>>();
        for (auto& r : found)
        {
            storage.push_back(cc::vector<char>::create_filled(isize(r.dataSize), '\0'));
            r.pData = storage.back().data();
        }
        if (ctx._get_pipeline_executable_representations(ctx._device, &which, &forms, found.data()) != VK_SUCCESS)
            continue;
        for (auto k = isize(0); k < found.size(); ++k)
        {
            auto const& r = found[k];
            auto text = cc::string();
            // a text form counts its terminating zero
            if (r.isText == VK_TRUE && r.dataSize > 0)
                text = cc::string(cc::string_view(storage[k].data(), isize(r.dataSize) - 1));
            out.push_back({.executable = cc::string(executables[e].name),
                           .name = cc::string(r.name),
                           .description = cc::string(r.description),
                           .text = cc::move(text)});
        }
    }
    return out;
}

cc::vector<sg::driver_representation> sg::backend::vulkan::vulkan_compute_pipeline::driver_representations() const
{
    return driver_representations_of(_ctx, _pipeline);
}

cc::vector<sg::driver_representation> sg::backend::vulkan::vulkan_raster_pipeline::driver_representations() const
{
    return driver_representations_of(_ctx, _pipeline);
}

cc::vector<sg::driver_statistic> sg::backend::vulkan::vulkan_compute_pipeline::driver_statistics() const
{
    return driver_statistics_of(_ctx, _pipeline);
}

cc::vector<sg::driver_statistic> sg::backend::vulkan::vulkan_raster_pipeline::driver_statistics() const
{
    return driver_statistics_of(_ctx, _pipeline);
}
