#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::vulkan
{
namespace
{
// Vulkan keeps sampled images and samplers as separate descriptor types, which sg's binding_type already does too —
// so this is one-to-one rather than the combined-descriptor translation some APIs need.
VkDescriptorType to_vk_descriptor_type(sg::binding_type t)
{
    switch (t)
    {
    case sg::binding_type::uniform_buffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case sg::binding_type::readonly_structured_buffer:
    case sg::binding_type::readwrite_structured_buffer:
    case sg::binding_type::readonly_raw_buffer:
    case sg::binding_type::readwrite_raw_buffer:
        // Vulkan has one storage-buffer type; read-only versus read-write is the shader's declaration and the
        // barrier's access, not a different descriptor.
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case sg::binding_type::readonly_texture:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case sg::binding_type::readwrite_texture:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case sg::binding_type::sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case sg::binding_type::acceleration_structure:
        return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    CC_UNREACHABLE("unhandled binding_type in to_vk_descriptor_type");
}
} // namespace

cc::result<vulkan_binding_group_layout_handle> vulkan_binding_group_layout::create(
    vulkan_context& ctx,
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers)
{
    auto const hash = sg::impl::binding_group_layout_hash(bindings, static_samplers);

    // One entry per binding slot: the static sampler declared for it, or null.
    // Owned by the layout, because every group built from it writes that sampler's descriptor into its own range.
    auto slot_samplers = cc::vector<VkSampler>::create_filled(bindings.size(), VkSampler(VK_NULL_HANDLE));
    auto const destroy_samplers = [&]
    {
        for (auto s : slot_samplers)
            if (s != VK_NULL_HANDLE)
                vkDestroySampler(ctx._device, s, nullptr);
    };

    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        CC_ASSERT(b.count >= 1, "unbounded / zero-count bindings are not supported yet");
        if (!sg::is_sampler(b.type))
            continue;

        // A sampler binding is static iff its name appears in the layout's static sampler list.
        for (auto const& s : static_samplers)
            if (s.name == b.name)
            {
                auto const info = to_vk_sampler_info(s.sampler);
                if (VkResult const r = vkCreateSampler(ctx._device, &info, nullptr, &slot_samplers[i]); r != VK_SUCCESS)
                {
                    destroy_samplers();
                    return vulkan_error(r, "vkCreateSampler (static sampler) failed");
                }
                break;
            }
    }

    // A Vulkan set numbers every descriptor in one namespace, so two bindings may not share an index.
    // HLSL has two more namespaces — the register space, and the register class behind t/s/u/b — and a layout
    // reflected from it routinely collides here.
    // Caught with the names rather than left to the validation layer, which reports a binding number and not which
    // two bindings own it.
    for (isize i = 0; i < bindings.size(); ++i)
        for (isize j = i + 1; j < bindings.size(); ++j)
            if (bindings[i].index == bindings[j].index)
            {
                destroy_samplers();
                return cc::error(cc::format("binding_group_layout: '{}' and '{}' are both at index {} — a vulkan "
                                            "descriptor set numbers every binding in one namespace, so a register "
                                            "space cannot tell two of them apart",
                                            bindings[i].name, bindings[j].name, bindings[i].index));
            }

    auto vk_bindings = cc::vector<VkDescriptorSetLayoutBinding>::create_uninitialized(bindings.size());
    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        vk_bindings[i] = VkDescriptorSetLayoutBinding{
            .binding = b.index,
            .descriptorType = to_vk_descriptor_type(b.type),
            .descriptorCount = b.count,
            // sg has no per-stage visibility on a binding, so every stage may read it.
            // Narrowing this is a pure optimization and needs a stage set sg does not carry.
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = nullptr,
        };
    }

    auto const info = VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        // A layout used with descriptor buffers has to say so at creation; it cannot also be allocated from a pool.
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = u32(vk_bindings.size()),
        .pBindings = vk_bindings.data(),
    };

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &info, nullptr, &layout); r != VK_SUCCESS)
    {
        destroy_samplers();
        return vulkan_error(r, "vkCreateDescriptorSetLayout failed");
    }

    // The size and the per-binding offsets are the device's answer, not ours: a descriptor's size is a device property,
    // and the driver is free to lay a set out with padding between bindings.
    VkDeviceSize layout_size = 0;
    ctx._descriptor_functions.get_layout_size(ctx._device, layout, &layout_size);

    auto binding_offsets = cc::vector<isize>::create_uninitialized(bindings.size());
    for (isize i = 0; i < bindings.size(); ++i)
    {
        VkDeviceSize offset = 0;
        ctx._descriptor_functions.get_binding_offset(ctx._device, layout, bindings[i].index, &offset);
        binding_offsets[i] = isize(offset);
    }

    auto owned = cc::vector<sg::binding>::create_copy_of(bindings);
    return vulkan_binding_group_layout_handle(std::make_shared<vulkan_binding_group_layout>(
        ctx, hash, cc::move(owned), layout, cc::move(slot_samplers), isize(layout_size), cc::move(binding_offsets)));
}

isize vulkan_binding_group_layout::descriptor_offset_of(isize slot, int element) const
{
    auto const& b = bindings()[slot];
    CC_ASSERT(element >= 0 && element < int(b.count), "element is outside the binding's count");
    return _binding_offsets[slot] + isize(element) * descriptor_size_of(_ctx, b.type);
}

vulkan_binding_group_layout::~vulkan_binding_group_layout()
{
    if (_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _layout, nullptr);
    for (auto sampler : _slot_samplers)
        if (sampler != VK_NULL_HANDLE)
            vkDestroySampler(_ctx._device, sampler, nullptr);
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_binding_group_layout_handle> vulkan_context::create_vulkan_binding_group_layout(
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers)
{
    return vulkan_binding_group_layout::create(*this, bindings, static_samplers);
}
} // namespace sg::backend::vulkan
