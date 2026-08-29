#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_pipeline_layout_handle> vulkan_pipeline_layout::create(vulkan_context& ctx,
                                                                         sg::pipeline_layout_description const& desc)
{
    auto const hash = sg::impl::pipeline_layout_hash(desc);

    // A group's position in the description is its bind slot, and the same index is the `firstSet` a bind command
    // passes — so the ordering here is the whole of what dx12 needs a root-parameter index table for.
    cc::vector<VkDescriptorSetLayout> set_layouts;
    cc::vector<sg::binding_group_layout_handle> groups;
    for (auto const& group : desc.groups)
    {
        CC_ASSERT(group != nullptr, "a pipeline layout's group is null");
        auto const vk_group = std::dynamic_pointer_cast<vulkan_binding_group_layout const>(group);
        CC_ASSERT(vk_group != nullptr, "binding group layout is not a vulkan one");
        set_layouts.push_back(vk_group->_layout);
        groups.push_back(group);
    }

    // Pipeline-level static samplers are not part of any group, so they cannot ride a set layout's immutable slot.
    // Vulkan has no pipeline-level sampler concept at all — the objects are created and owned here so the layout can
    // hand them to whatever binds them, which is what keeps the sg surface honest until that path exists.
    cc::vector<VkSampler> static_samplers;
    for (auto const& s : desc.static_samplers)
    {
        auto const info = to_vk_sampler_info(s.sampler);
        VkSampler sampler = VK_NULL_HANDLE;
        if (VkResult const r = vkCreateSampler(ctx._device, &info, nullptr, &sampler); r != VK_SUCCESS)
        {
            for (auto created : static_samplers)
                vkDestroySampler(ctx._device, created, nullptr);
            return vulkan_error(r, "vkCreateSampler (pipeline static sampler) failed");
        }
        static_samplers.push_back(sampler);
    }

    // Inline constants become one push-constant range visible to every stage, matching how the binding itself is
    // declared: sg carries no stage mask on it.
    int inline_bytes = 0;
    VkPushConstantRange push_range = {};
    if (desc.inline_constants.has_value())
    {
        auto const& b = desc.inline_constants.value();
        CC_ASSERT(b.type == sg::binding_type::uniform_buffer, "inline constants must be a uniform_buffer binding");
        CC_ASSERT(b.block_size.has_value() && b.block_size.value() > 0 && b.block_size.value() % 4 == 0,
                  "inline constants need a block_size that is a positive multiple of 4");
        inline_bytes = int(b.block_size.value());
        push_range = VkPushConstantRange{
            .stageFlags = VK_SHADER_STAGE_ALL,
            .offset = 0,
            .size = u32(inline_bytes),
        };
    }

    auto const info = VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = u32(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = inline_bytes > 0 ? 1u : 0u,
        .pPushConstantRanges = inline_bytes > 0 ? &push_range : nullptr,
    };

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (VkResult const r = vkCreatePipelineLayout(ctx._device, &info, nullptr, &layout); r != VK_SUCCESS)
    {
        for (auto created : static_samplers)
            vkDestroySampler(ctx._device, created, nullptr);
        return vulkan_error(r, "vkCreatePipelineLayout failed");
    }

    return vulkan_pipeline_layout_handle(std::make_shared<vulkan_pipeline_layout>(
        ctx, hash, layout, cc::move(groups), cc::move(static_samplers), inline_bytes));
}

vulkan_pipeline_layout::~vulkan_pipeline_layout()
{
    if (_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(_ctx._device, _layout, nullptr);
    for (auto sampler : _static_samplers)
        vkDestroySampler(_ctx._device, sampler, nullptr);
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_pipeline_layout_handle> vulkan_context::create_vulkan_pipeline_layout(
    sg::pipeline_layout_description const& desc)
{
    return vulkan_pipeline_layout::create(*this, desc);
}
} // namespace sg::backend::vulkan
