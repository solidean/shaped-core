#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

namespace sg::backend::vulkan
{
namespace
{
/// One shader module plus its entry-point string, kept together because the stage struct points at both.
/// Collected in full before any stage struct is built — see the note in vulkan_raster_pipeline.cc.
struct staged_module
{
    VkShaderModule module = VK_NULL_HANDLE;
    cc::string entry_point;
};

[[nodiscard]] VkShaderStageFlagBits to_vk_rt_stage(sg::shader_stage s)
{
    switch (s)
    {
    case sg::shader_stage::raygen:
        return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case sg::shader_stage::miss:
        return VK_SHADER_STAGE_MISS_BIT_KHR;
    case sg::shader_stage::closest_hit:
        return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case sg::shader_stage::any_hit:
        return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case sg::shader_stage::intersection:
        return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case sg::shader_stage::callable:
        return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    default:
        CC_UNREACHABLE("not a ray-tracing shader stage");
    }
}
} // namespace

cc::result<vulkan_raytracing_pipeline_handle> vulkan_raytracing_pipeline::create(
    vulkan_context& ctx,
    sg::raytracing_pipeline_description const& desc)
{
    CC_ASSERT(ctx.is_raytracing_supported(), "ray tracing is not supported on this device");
    CC_ASSERT(desc.layout != nullptr, "raytracing pipeline requires a pipeline_layout");
    CC_ASSERT(!desc.raygen_shaders.empty(), "a raytracing pipeline needs at least one raygen shader");
    CC_ASSERT(desc.max_recursion_depth >= 1, "max_recursion_depth must be >= 1");

    auto vk_layout = std::dynamic_pointer_cast<vulkan_pipeline_layout const>(desc.layout);
    CC_ASSERT(vk_layout != nullptr, "pipeline_layout is not a vulkan pipeline_layout");

    auto pipeline = std::make_shared<vulkan_raytracing_pipeline>(ctx);
    pipeline->layout = cc::move(vk_layout);

    cc::vector<staged_module> modules;
    cc::vector<VkShaderStageFlagBits> stage_bits;
    CC_DEFER
    {
        for (auto const& m : modules)
            vkDestroyShaderModule(ctx._device, m.module, nullptr);
    };

    // Returns the stage index a group will name, or -1 for an absent optional stage.
    auto const add_stage = [&](sg::compiled_shader const& shader) -> int
    {
        CC_ASSERT(shader.format == sg::shader_format::spirv, "the vulkan backend requires SPIR-V bytecode");
        CC_ASSERT(!shader.bytecode.empty(), "a ray-tracing shader has no bytecode");
        CC_ASSERT(shader.bytecode.size() % 4 == 0, "SPIR-V bytecode must be a whole number of 32-bit words");

        auto const info = VkShaderModuleCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = size_t(shader.bytecode.size()),
            .pCode = reinterpret_cast<u32 const*>(shader.bytecode.data()),
        };
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx._device, &info, nullptr, &module) != VK_SUCCESS)
            return -1;

        modules.push_back(
            {.module = module, .entry_point = cc::string::create_copy_c_str_materialized(shader.entry_point)});
        stage_bits.push_back(to_vk_rt_stage(shader.stage));
        return int(modules.size()) - 1;
    };

    // Groups in the order sg registers them, so a handle indexes straight into its category's run.
    cc::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    auto const general_group = [&](int stage_index)
    {
        groups.push_back(VkRayTracingShaderGroupCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = u32(stage_index),
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
        });
    };

    pipeline->_raygen_base = int(groups.size());
    for (auto const& s : desc.raygen_shaders)
    {
        int const idx = add_stage(s);
        if (idx < 0)
            return cc::error("vkCreateShaderModule failed for a raygen shader");
        general_group(idx);
    }

    pipeline->_miss_base = int(groups.size());
    for (auto const& s : desc.miss_shaders)
    {
        int const idx = add_stage(s);
        if (idx < 0)
            return cc::error("vkCreateShaderModule failed for a miss shader");
        general_group(idx);
    }

    pipeline->_hit_base = int(groups.size());
    for (auto const& h : desc.hit_shaders)
    {
        // An intersection stage is what makes the group procedural, which is sg's own rule rather than Vulkan's
        // spelling of it.
        bool const procedural = h.intersection.has_value();
        auto group = VkRayTracingShaderGroupCreateInfoKHR{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = procedural ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                               : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = VK_SHADER_UNUSED_KHR,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
        };
        if (h.closest_hit.has_value())
        {
            int const idx = add_stage(h.closest_hit.value());
            if (idx < 0)
                return cc::error("vkCreateShaderModule failed for a closest-hit shader");
            group.closestHitShader = u32(idx);
        }
        if (h.any_hit.has_value())
        {
            int const idx = add_stage(h.any_hit.value());
            if (idx < 0)
                return cc::error("vkCreateShaderModule failed for an any-hit shader");
            group.anyHitShader = u32(idx);
        }
        if (h.intersection.has_value())
        {
            int const idx = add_stage(h.intersection.value());
            if (idx < 0)
                return cc::error("vkCreateShaderModule failed for an intersection shader");
            group.intersectionShader = u32(idx);
        }
        groups.push_back(group);
    }

    pipeline->_callable_base = int(groups.size());
    for (auto const& s : desc.callable_shaders)
    {
        int const idx = add_stage(s);
        if (idx < 0)
            return cc::error("vkCreateShaderModule failed for a callable shader");
        general_group(idx);
    }
    pipeline->_group_count = int(groups.size());

    // Now that `modules` has stopped growing, its entry-point strings have stable addresses.
    auto stages = cc::vector<VkPipelineShaderStageCreateInfo>::create_uninitialized(modules.size());
    for (isize i = 0; i < modules.size(); ++i)
        stages[i] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage_bits[i],
            .module = modules[i].module,
            .pName = modules[i].entry_point.c_str_if_terminated(),
        };

    auto const& rt_props = ctx.raytracing_pipeline_properties();
    CC_ASSERTF(desc.max_recursion_depth <= rt_props.maxRayRecursionDepth,
               "max_recursion_depth {} exceeds the device's limit of {}", desc.max_recursion_depth,
               rt_props.maxRayRecursionDepth);

    auto const info = VkRayTracingPipelineCreateInfoKHR{
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stageCount = u32(stages.size()),
        .pStages = stages.data(),
        .groupCount = u32(groups.size()),
        .pGroups = groups.data(),
        // sg's max_payload_size / max_attribute_size have no create-info counterpart: SPIR-V declares both on the
        // shader itself, so the limits travel in the module rather than beside it.
        .maxPipelineRayRecursionDepth = desc.max_recursion_depth,
        .layout = pipeline->layout->_layout,
    };

    if (VkResult const r = ctx._raytracing_functions.create_raytracing_pipelines(
            ctx._device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline->_pipeline);
        r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateRayTracingPipelinesKHR failed");

    // Read every group's handle once, so building a shader table is a memcpy.
    pipeline->_handle_size = isize(rt_props.shaderGroupHandleSize);
    pipeline->_group_handles
        = cc::vector<byte>::create_uninitialized(pipeline->_handle_size * isize(pipeline->_group_count));
    if (VkResult const r = ctx._raytracing_functions.get_shader_group_handles(
            ctx._device, pipeline->_pipeline, 0, u32(pipeline->_group_count), size_t(pipeline->_group_handles.size()),
            pipeline->_group_handles.data());
        r != VK_SUCCESS)
        return vulkan_error(r, "vkGetRayTracingShaderGroupHandlesKHR failed");

    return vulkan_raytracing_pipeline_handle(cc::move(pipeline));
}

namespace
{
[[nodiscard]] cc::span<byte const> handle_at(cc::vector<byte> const& handles, isize handle_size, int base, int index, int count)
{
    CC_ASSERT(index >= 0 && index < count, "shader handle is not one of this pipeline's");
    return cc::span<byte const>(handles.data() + isize(base + index) * handle_size, handle_size);
}
} // namespace

cc::span<byte const> vulkan_raytracing_pipeline::raygen_handle(sg::raygen_shader_handle h) const
{
    return handle_at(_group_handles, _handle_size, _raygen_base, int(h), _miss_base - _raygen_base);
}
cc::span<byte const> vulkan_raytracing_pipeline::miss_handle(sg::miss_shader_handle h) const
{
    return handle_at(_group_handles, _handle_size, _miss_base, int(h), _hit_base - _miss_base);
}
cc::span<byte const> vulkan_raytracing_pipeline::hit_handle(sg::hit_shader_handle h) const
{
    return handle_at(_group_handles, _handle_size, _hit_base, int(h), _callable_base - _hit_base);
}
cc::span<byte const> vulkan_raytracing_pipeline::callable_handle(sg::callable_shader_handle h) const
{
    return handle_at(_group_handles, _handle_size, _callable_base, int(h), _group_count - _callable_base);
}

void vulkan_raytracing_pipeline::release_backend_objects()
{
    if (_pipeline == VK_NULL_HANDLE)
        return;

    vulkan_expiring_resource expiring;
    auto* const device = &_ctx._device;
    expiring.finalizers.push_back([device, pipeline = _pipeline] { vkDestroyPipeline(*device, pipeline, nullptr); });
    _pipeline = VK_NULL_HANDLE;
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

vulkan_raytracing_pipeline::~vulkan_raytracing_pipeline()
{
    this->release_backend_objects();
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_raytracing_pipeline_handle> vulkan_context::create_vulkan_raytracing_pipeline(
    sg::raytracing_pipeline_description const& desc)
{
    return vulkan_raytracing_pipeline::create(*this, desc);
}
} // namespace sg::backend::vulkan
