#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
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
    // The cap is sg's rather than vulkan's: a caller gets max_binding_groups slots on every backend, and the one
    // above them is sg::reserved_binding_group.
    // A small_vector still heap-grows past its inline size, so this is a real check rather than a restatement.
    if (int(desc.groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    auto const hash = sg::impl::pipeline_layout_hash(desc);

    auto bound_samplers = cc::vector<VkSampler>();
    auto reserved_set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    auto empty_set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    auto const destroy_reserved = [&]
    {
        if (reserved_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx._device, reserved_set_layout, nullptr);
        if (empty_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx._device, empty_set_layout, nullptr);
    };

    if (!desc.static_samplers.empty())
    {
        auto reserved_bindings = cc::vector<VkDescriptorSetLayoutBinding>();
        for (isize i = 0; i < desc.static_samplers.size(); ++i)
        {
            // sg refused a malformed or colliding one before the backend saw it.
            auto const& s = desc.static_samplers[i];

            // The context's cache owns the sampler, and outlives every in-flight use of it.
            auto const sampler = ctx._samplers.acquire(s.sampler);
            if (sampler == VK_NULL_HANDLE)
            {
                destroy_reserved();
                return cc::error(
                    cc::format("pipeline_layout: could not create the bound sampler for '{}'", s.binding.name));
            }
            bound_samplers.push_back(sampler);
        }
        // Filled only once every sampler exists, since a binding points into `bound_samplers`.
        for (isize i = 0; i < desc.static_samplers.size(); ++i)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = desc.static_samplers[i].binding.index + 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = &bound_samplers[i],
            });

        auto const reserved_info = VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
                   | VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT,
            .bindingCount = u32(reserved_bindings.size()),
            .pBindings = reserved_bindings.data(),
        };
        if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &reserved_info, nullptr, &reserved_set_layout);
            r != VK_SUCCESS)
        {
            destroy_reserved();
            return vulkan_error(r, "vkCreateDescriptorSetLayout (reserved set) failed");
        }

        auto const empty_info = VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        };
        if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &empty_info, nullptr, &empty_set_layout);
            r != VK_SUCCESS)
        {
            destroy_reserved();
            return vulkan_error(r, "vkCreateDescriptorSetLayout (empty set) failed");
        }
    }

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
    if (reserved_set_layout != VK_NULL_HANDLE)
    {
        while (set_layouts.size() < sg::reserved_binding_group)
            set_layouts.push_back(empty_set_layout);
        set_layouts.push_back(reserved_set_layout);
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
    {
        destroy_reserved();
        return vulkan_error(r, "vkCreatePipelineLayout failed");
    }

    auto result = std::make_shared<vulkan_pipeline_layout>(ctx, hash, layout, cc::move(groups), desc.inline_constants,
                                                           inline_bytes);
    result->_reserved_set_layout = reserved_set_layout;
    result->_empty_set_layout = empty_set_layout;
    return vulkan_pipeline_layout_handle(cc::move(result));
}

void vulkan_pipeline_layout::bind_embedded_samplers(VkCommandBuffer buffer, VkPipelineBindPoint bind_point) const
{
    if (_reserved_set_layout != VK_NULL_HANDLE)
        _ctx._descriptor_functions.cmd_bind_embedded_samplers(buffer, bind_point, _layout,
                                                              u32(sg::reserved_binding_group));
}

// Immediate rather than epoch-deferred: the layout objects are consumed at pipeline-creation and descriptor-allocation time, so no in-flight work names them.
void vulkan_pipeline_layout::release_backend_objects()
{
    if (_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(_ctx._device, _layout, nullptr);
    if (_reserved_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _reserved_set_layout, nullptr);
    if (_empty_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _empty_set_layout, nullptr);
    _layout = VK_NULL_HANDLE;
    _reserved_set_layout = VK_NULL_HANDLE;
    _empty_set_layout = VK_NULL_HANDLE;
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
