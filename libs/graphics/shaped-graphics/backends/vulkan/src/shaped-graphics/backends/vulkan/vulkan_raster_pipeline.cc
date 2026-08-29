#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/vulkan_compute_pipeline.hh> // is_usable_pipeline_cache_blob
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_state.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh> // to_vk_compare_op
#include <shaped-graphics/binding/compiled_shader.hh>

namespace sg::backend::vulkan
{
namespace
{
/// One shader module plus the entry-point string it names, kept together because the stage struct points at both.
struct staged_module
{
    VkShaderModule module = VK_NULL_HANDLE;
    cc::string entry_point;
};

cc::result<staged_module> make_module(vulkan_context& ctx, sg::compiled_shader const& shader, char const* what)
{
    CC_ASSERTF(shader.format == sg::shader_format::spirv, "the vulkan backend requires SPIR-V bytecode ({})", what);
    CC_ASSERTF(!shader.bytecode.empty(), "{} has no bytecode", what);
    CC_ASSERTF(shader.bytecode.size() % 4 == 0, "{} bytecode must be a whole number of 32-bit words", what);

    auto const info = VkShaderModuleCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size_t(shader.bytecode.size()),
        .pCode = reinterpret_cast<u32 const*>(shader.bytecode.data()),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateShaderModule(ctx._device, &info, nullptr, &module); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateShaderModule failed");
    return staged_module{.module = module, .entry_point = cc::string::create_copy_c_str_materialized(shader.entry_point)};
}

VkStencilOpState to_vk_stencil_face(sg::stencil_face const& f, sg::depth_stencil_state const& ds)
{
    return VkStencilOpState{
        .failOp = to_vk_stencil_op(f.fail),
        .passOp = to_vk_stencil_op(f.pass),
        .depthFailOp = to_vk_stencil_op(f.depth_fail),
        .compareOp = to_vk_compare_op(f.compare),
        .compareMask = ds.stencil_read_mask,
        .writeMask = ds.stencil_write_mask,
        // The reference is dynamic (cmd.raster.set_stencil_reference), so the pipeline states none.
        .reference = 0,
    };
}
} // namespace

cc::result<vulkan_raster_pipeline_handle> vulkan_raster_pipeline::create(vulkan_context& ctx,
                                                                         sg::raster_pipeline_description const& desc)
{
    CC_ASSERT(desc.layout != nullptr, "raster pipeline requires a pipeline_layout");
    CC_ASSERT(desc.vertex_shader.stage == sg::shader_stage::vertex, "the vertex stage must be a vertex shader");

    auto vk_layout = std::dynamic_pointer_cast<vulkan_pipeline_layout const>(desc.layout);
    CC_ASSERT(vk_layout != nullptr, "pipeline_layout is not a vulkan pipeline_layout");

    auto pipeline = std::make_shared<vulkan_raster_pipeline>(ctx);
    pipeline->layout = cc::move(vk_layout);

    bool const usable = !desc.cached_pipeline.empty() && is_usable_pipeline_cache_blob(ctx, desc.cached_pipeline.span());
    auto const cache_info = VkPipelineCacheCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = usable ? size_t(desc.cached_pipeline.size()) : 0,
        .pInitialData = usable ? desc.cached_pipeline.data() : nullptr,
    };
    if (VkResult const r = vkCreatePipelineCache(ctx._device, &cache_info, nullptr, &pipeline->_cache); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreatePipelineCache failed");
    pipeline->_used_cached_pipeline = usable;

    // Every stage's module, kept alive together until vkCreateGraphicsPipelines has consumed them.
    //
    // Collected in full before any VkPipelineShaderStageCreateInfo is built, because a stage's `pName` points into the
    // module entry it came from — and a vector that grows while stages are being appended would leave the earlier
    // pointers dangling.
    cc::vector<staged_module> modules;
    cc::vector<VkShaderStageFlagBits> stage_bits;
    CC_DEFER
    {
        for (auto const& m : modules)
            vkDestroyShaderModule(ctx._device, m.module, nullptr);
    };

    auto const add_stage
        = [&](sg::compiled_shader const& shader, VkShaderStageFlagBits stage, char const* what) -> cc::result<cc::unit>
    {
        auto m = make_module(ctx, shader, what);
        CC_RETURN_IF_ERROR(m);
        modules.push_back(cc::move(m.value()));
        stage_bits.push_back(stage);
        return cc::unit{};
    };

    {
        auto r = add_stage(desc.vertex_shader, VK_SHADER_STAGE_VERTEX_BIT, "vertex shader");
        CC_RETURN_IF_ERROR(r);
    }
    if (desc.tessellation_control_shader.has_value())
    {
        CC_ASSERT(desc.tessellation_evaluation_shader.has_value(), "a tessellator needs both a control and an "
                                                                   "evaluation stage");
        auto r = add_stage(desc.tessellation_control_shader.value(), VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           "tessellation control shader");
        CC_RETURN_IF_ERROR(r);
    }
    if (desc.tessellation_evaluation_shader.has_value())
    {
        CC_ASSERT(desc.tessellation_control_shader.has_value(), "a tessellator needs both a control and an evaluation "
                                                                "stage");
        auto r = add_stage(desc.tessellation_evaluation_shader.value(), VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           "tessellation evaluation shader");
        CC_RETURN_IF_ERROR(r);
    }
    if (desc.geometry_shader.has_value())
    {
        auto r = add_stage(desc.geometry_shader.value(), VK_SHADER_STAGE_GEOMETRY_BIT, "geometry shader");
        CC_RETURN_IF_ERROR(r);
    }
    if (desc.fragment_shader.has_value())
    {
        auto r = add_stage(desc.fragment_shader.value(), VK_SHADER_STAGE_FRAGMENT_BIT, "fragment shader");
        CC_RETURN_IF_ERROR(r);
    }

    // Now that `modules` has stopped growing, its entry-point strings have stable addresses.
    auto stages = cc::vector<VkPipelineShaderStageCreateInfo>::create_uninitialized(modules.size());
    for (isize i = 0; i < modules.size(); ++i)
        stages[i] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage_bits[i],
            .module = modules[i].module,
            .pName = modules[i].entry_point.c_str_if_terminated(),
        };

    // Vertex input.
    //
    // WORKAROUND: an attribute's SPIR-V location is its index in `attributes`, because sg identifies a vertex input by
    // an HLSL `semantic` string and SPIR-V has no such thing.
    // So a Vulkan-targeted shader must annotate `[[vk::location(N)]]` to match the layout's attribute order.
    // The replacement is the backend-neutral numeric `location` on sg::vertex_attribute that the TODO list already
    // names — with it, this line reads the field instead of counting.
    // See libs/graphics/shaped-graphics/docs/TODO.md.
    cc::vector<VkVertexInputBindingDescription> vk_slots;
    for (isize i = 0; i < desc.vertex_input.slots.size(); ++i)
    {
        auto const& s = desc.vertex_input.slots[i];
        vk_slots.push_back(VkVertexInputBindingDescription{
            .binding = u32(i),
            .stride = u32(s.stride),
            .inputRate = s.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
        });
    }
    cc::vector<VkVertexInputAttributeDescription> vk_attributes;
    for (isize i = 0; i < desc.vertex_input.attributes.size(); ++i)
    {
        auto const& a = desc.vertex_input.attributes[i];
        vk_attributes.push_back(VkVertexInputAttributeDescription{
            .location = u32(i),
            .binding = u32(a.slot),
            .format = to_vk_vertex_format(a.format),
            .offset = u32(a.offset),
        });
    }
    auto const vertex_input = VkPipelineVertexInputStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = u32(vk_slots.size()),
        .pVertexBindingDescriptions = vk_slots.data(),
        .vertexAttributeDescriptionCount = u32(vk_attributes.size()),
        .pVertexAttributeDescriptions = vk_attributes.data(),
    };

    auto const input_assembly = VkPipelineInputAssemblyStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = to_vk_topology(desc.topology),
        .primitiveRestartEnable = VK_FALSE,
    };

    auto const tessellation = VkPipelineTessellationStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = u32(desc.patch_control_points),
    };
    bool const has_tessellation = desc.tessellation_control_shader.has_value();
    if (has_tessellation)
    {
        CC_ASSERT(desc.topology == sg::primitive_topology::patch_list, "a tessellation pipeline must use patch_list");
        CC_ASSERT(desc.patch_control_points >= 1 && desc.patch_control_points <= 32, "patch_control_points must be "
                                                                                     "1..32");
    }

    // Counts only: the viewport and scissor themselves are dynamic, which is what makes them commands at the sg level.
    auto const viewport_state = VkPipelineViewportStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    auto const& rs = desc.rasterization;
    auto const rasterization = VkPipelineRasterizationStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        // depthClampEnable is the inverse of sg's depth_clip_enabled: D3D12 spells the same knob as DepthClipEnable,
        // and Vulkan spells its opposite.
        .depthClampEnable = rs.depth_clip_enabled ? VK_FALSE : VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = to_vk_polygon_mode(rs.fill),
        .cullMode = to_vk_cull_mode(rs.cull),
        .frontFace = to_vk_front_face(rs.front),
        .depthBiasEnable
        = (rs.depth_bias != 0.0f || rs.depth_bias_slope != 0.0f || rs.depth_bias_clamp != 0.0f) ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = rs.depth_bias,
        .depthBiasClamp = rs.depth_bias_clamp,
        .depthBiasSlopeFactor = rs.depth_bias_slope,
        .lineWidth = 1.0f, // required to be 1.0 without the wideLines feature, which sg exposes no knob for
    };

    auto const multisample = VkPipelineMultisampleStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VkSampleCountFlagBits(desc.sample_count),
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    auto const& ds = desc.depth_stencil;
    auto const depth_stencil = VkPipelineDepthStencilStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ds.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = ds.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = to_vk_compare_op(ds.depth_compare),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = ds.stencil_test ? VK_TRUE : VK_FALSE,
        .front = to_vk_stencil_face(ds.front, ds),
        .back = to_vk_stencil_face(ds.back, ds),
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };

    cc::vector<VkPipelineColorBlendAttachmentState> attachments;
    cc::vector<VkFormat> color_formats;
    for (auto const& ct : desc.color_targets)
    {
        color_formats.push_back(to_vk_format(ct.format));
        auto state = VkPipelineColorBlendAttachmentState{
            .blendEnable = ct.blend.has_value() ? VK_TRUE : VK_FALSE,
            .colorWriteMask = to_vk_color_write_mask(ct.write_mask),
        };
        if (ct.blend.has_value())
        {
            auto const& b = ct.blend.value();
            state.srcColorBlendFactor = to_vk_blend_factor(b.color.source);
            state.dstColorBlendFactor = to_vk_blend_factor(b.color.target);
            state.colorBlendOp = to_vk_blend_op(b.color.op);
            state.srcAlphaBlendFactor = to_vk_blend_factor(b.alpha.source);
            state.dstAlphaBlendFactor = to_vk_blend_factor(b.alpha.target);
            state.alphaBlendOp = to_vk_blend_op(b.alpha.op);
        }
        attachments.push_back(state);
    }
    auto const color_blend = VkPipelineColorBlendStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = u32(attachments.size()),
        .pAttachments = attachments.data(),
        // Dynamic, so these are ignored — cmd.raster.set_blend_constants is the sg spelling.
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };

    VkDynamicState const dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    auto const dynamic = VkPipelineDynamicStateCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = u32(sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
    };

    // Dynamic rendering: the target formats travel with the pipeline, which is exactly the shape
    // raster_pipeline_description already has.
    // A depth-stencil format that carries stencil is named twice, since Vulkan asks for the two aspects separately.
    auto const depth_format = to_vk_format(desc.depth_stencil_format);
    auto const rendering = VkPipelineRenderingCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = u32(color_formats.size()),
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = sg::has_stencil(desc.depth_stencil_format) ? depth_format : VK_FORMAT_UNDEFINED,
    };

    auto const info = VkGraphicsPipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stageCount = u32(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = has_tessellation ? &tessellation : nullptr,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic,
        .layout = pipeline->layout->_layout,
        // No render pass and no subpass: that is what VkPipelineRenderingCreateInfo above replaces.
        .renderPass = VK_NULL_HANDLE,
    };

    if (VkResult const r
        = vkCreateGraphicsPipelines(ctx._device, pipeline->_cache, 1, &info, nullptr, &pipeline->_pipeline);
        r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateGraphicsPipelines failed");

    return vulkan_raster_pipeline_handle(cc::move(pipeline));
}

cc::pinned_data<byte const> vulkan_raster_pipeline::cached_pipeline_data() const
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

vulkan_raster_pipeline::~vulkan_raster_pipeline()
{
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
cc::result<vulkan_raster_pipeline_handle> vulkan_context::create_vulkan_raster_pipeline(
    sg::raster_pipeline_description const& desc)
{
    return vulkan_raster_pipeline::create(*this, desc);
}
} // namespace sg::backend::vulkan
