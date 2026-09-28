#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_pipeline_layout_handle> vulkan_pipeline_layout::create(vulkan_context& ctx,
                                                                         sg::pipeline_layout_description const& desc)
{
    // The cap is sg's rather than vulkan's: a caller gets max_binding_groups slots on every backend, and the one
    // above them is sg::reserved_binding_group.
    // A small_vector still heap-grows past its inline size, so this is a real check rather than a restatement.
    if (int(desc.groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    // Refused rather than accepted: this backend binds no pipeline-level sampler to a set a shader could read.
    // The gap is libs/graphics/shaped-graphics/docs/TODO.md's, and a group's name-matched static sampler is the working form.
    if (!desc.static_samplers.empty())
        return cc::error("pipeline_layout: a pipeline-level static sampler (bound_sampler) is not bound by the vulkan "
                         "backend yet; declare it a group's static sampler instead");

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

    // Inline constants become one push-constant range visible to every stage, matching how the binding itself is
    // declared: sg carries no stage mask on it.
    int inline_bytes = 0;
    VkPushConstantRange push_range = {};
    if (desc.inline_constants.has_value())
    {
        auto const& b = desc.inline_constants.value();
        CC_ASSERT(b.type == sg::binding_type::constants_buffer, "inline constants must be a constants_buffer binding");
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
        return vulkan_error(r, "vkCreatePipelineLayout failed");

    return vulkan_pipeline_layout_handle(std::make_shared<vulkan_pipeline_layout>(ctx, hash, layout, cc::move(groups),
                                                                                  desc.inline_constants, inline_bytes));
}

// Immediate rather than epoch-deferred, unchanged from what the destructor always did: a layout is consumed at
// pipeline-creation and descriptor-allocation time, so no in-flight work names it.
void vulkan_pipeline_layout::release_backend_objects()
{
    if (_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(_ctx._device, _layout, nullptr);
    _layout = VK_NULL_HANDLE;
}

vulkan_pipeline_layout::~vulkan_pipeline_layout()
{
    this->release_backend_objects();
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
