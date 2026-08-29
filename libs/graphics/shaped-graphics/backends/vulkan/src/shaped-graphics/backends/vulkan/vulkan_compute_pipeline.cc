#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <shaped-graphics/backends/vulkan/vulkan_compute_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

namespace sg::backend::vulkan
{
bool is_usable_pipeline_cache_blob(vulkan_context const& ctx, cc::span<byte const> blob)
{
    // Everything a driver checks before deciding to start from an empty cache instead.
    // The header's layout is specified precisely so an application can run the same check, which is what makes this an
    // exact answer rather than a guess.
    if (blob.size() < isize(sizeof(VkPipelineCacheHeaderVersionOne)))
        return false;

    VkPipelineCacheHeaderVersionOne header = {};
    cc::memcpy(&header, blob.data(), sizeof(header));

    auto const& props = ctx.device_properties();
    if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
        return false;
    if (header.vendorID != props.vendorID || header.deviceID != props.deviceID)
        return false;
    for (int i = 0; i < VK_UUID_SIZE; ++i)
        if (header.pipelineCacheUUID[i] != props.pipelineCacheUUID[i])
            return false;
    return true;
}

cc::result<vulkan_compute_pipeline_handle> vulkan_compute_pipeline::create(vulkan_context& ctx,
                                                                           vulkan_pipeline_layout_handle layout,
                                                                           sg::compiled_shader const& shader,
                                                                           cc::span<byte const> cached_pipeline)
{
    CC_ASSERT(layout != nullptr, "compute pipeline requires a pipeline_layout");
    CC_ASSERT(shader.stage == sg::shader_stage::compute, "compute pipeline requires a compute shader");
    CC_ASSERT(shader.format == sg::shader_format::spirv, "the vulkan backend requires SPIR-V bytecode");
    CC_ASSERT(!shader.bytecode.empty(), "compute shader has no bytecode");
    CC_ASSERT(shader.bytecode.size() % 4 == 0, "SPIR-V bytecode must be a whole number of 32-bit words");
    CC_ASSERT(shader.workgroup_size.has_value(), "a compute shader must report its workgroup size");

    auto pipeline = std::make_shared<vulkan_compute_pipeline>(ctx, shader.workgroup_size.value());
    pipeline->layout = cc::move(layout);

    // The cache exists whether or not a blob was supplied, so that cached_pipeline_data() has something to serialize.
    // A blob this device would ignore is dropped here rather than handed over, which is what lets
    // used_cached_pipeline() answer exactly — see is_usable_pipeline_cache_blob.
    bool const usable = !cached_pipeline.empty() && is_usable_pipeline_cache_blob(ctx, cached_pipeline);
    auto const cache_info = VkPipelineCacheCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = usable ? size_t(cached_pipeline.size()) : 0,
        .pInitialData = usable ? cached_pipeline.data() : nullptr,
    };
    if (VkResult const r = vkCreatePipelineCache(ctx._device, &cache_info, nullptr, &pipeline->_cache); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreatePipelineCache failed");
    pipeline->_used_cached_pipeline = usable;

    // The module is a creation-time input only: the pipeline holds no reference to it once built, so it goes as soon
    // as vkCreateComputePipelines returns.
    auto const module_info = VkShaderModuleCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size_t(shader.bytecode.size()),
        .pCode = reinterpret_cast<u32 const*>(shader.bytecode.data()),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateShaderModule(ctx._device, &module_info, nullptr, &module); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateShaderModule failed");
    CC_DEFER
    {
        vkDestroyShaderModule(ctx._device, module, nullptr);
    };

    // Vulkan takes the entry point as a C string, and it must outlive the create call below.
    auto const entry_point = cc::string::create_copy_c_str_materialized(shader.entry_point);
    auto const info = VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        // A pipeline built against a descriptor-buffer set layout has to say so, and may not be used with sets.
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = entry_point.c_str_if_terminated()},
        .layout = pipeline->layout->_layout,
    };
    if (VkResult const r
        = vkCreateComputePipelines(ctx._device, pipeline->_cache, 1, &info, nullptr, &pipeline->_pipeline);
        r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateComputePipelines failed");

    return vulkan_compute_pipeline_handle(cc::move(pipeline));
}

cc::pinned_data<byte const> vulkan_compute_pipeline::cached_pipeline_data() const
{
    if (_cache == VK_NULL_HANDLE)
        return {};

    size_t size = 0;
    if (vkGetPipelineCacheData(_ctx._device, _cache, &size, nullptr) != VK_SUCCESS || size == 0)
        return {};

    auto data = cc::pinned_data<byte>::create_uninitialized(isize(size));
    if (vkGetPipelineCacheData(_ctx._device, _cache, &size, data.data()) != VK_SUCCESS)
        return {};
    return data;
}

vulkan_compute_pipeline::~vulkan_compute_pipeline()
{
    // The pipeline may still back in-flight work, so it goes through the epoch; the cache backs none and could go
    // now, but it rides along so that both releases are one staged entry.
    if (_pipeline == VK_NULL_HANDLE && _cache == VK_NULL_HANDLE)
        return;

    vulkan_expiring_resource expiring;
    auto* const device = &_ctx._device;
    expiring.finalizers.push_back(
        [device, pipeline = _pipeline, cache = _cache]
        {
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(*device, pipeline, nullptr);
            if (cache != VK_NULL_HANDLE)
                vkDestroyPipelineCache(*device, cache, nullptr);
        });
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_compute_pipeline_handle> vulkan_context::create_vulkan_compute_pipeline(
    sg::compute_pipeline_description const& desc)
{
    auto vk_layout = std::dynamic_pointer_cast<vulkan_pipeline_layout const>(desc.layout);
    CC_ASSERT(vk_layout != nullptr, "pipeline_layout is not a vulkan pipeline_layout");
    return vulkan_compute_pipeline::create(*this, cc::move(vk_layout), desc.shader, desc.cached_pipeline.span());
}
} // namespace sg::backend::vulkan
