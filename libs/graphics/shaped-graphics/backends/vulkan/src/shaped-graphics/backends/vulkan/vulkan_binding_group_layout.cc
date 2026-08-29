#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
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

    cc::vector<VkSampler> immutable_samplers;
    cc::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    cc::vector<VkSampler> per_binding_immutable; // one entry per binding; null unless it is a static sampler

    for (auto const& b : bindings)
    {
        CC_ASSERT(b.count >= 1, "unbounded / zero-count bindings are not supported yet");

        VkSampler immutable = VK_NULL_HANDLE;
        if (sg::is_sampler(b.type))
        {
            // A sampler binding is static iff its name appears in the layout's static sampler list.
            for (auto const& s : static_samplers)
                if (s.name == b.name)
                {
                    auto const info = to_vk_sampler_info(s.sampler);
                    if (VkResult const r = vkCreateSampler(ctx._device, &info, nullptr, &immutable); r != VK_SUCCESS)
                    {
                        for (auto sampler : immutable_samplers)
                            vkDestroySampler(ctx._device, sampler, nullptr);
                        return vulkan_error(r, "vkCreateSampler (static sampler) failed");
                    }
                    immutable_samplers.push_back(immutable);
                    break;
                }
        }
        per_binding_immutable.push_back(immutable);
    }

    for (isize i = 0; i < isize(bindings.size()); ++i)
    {
        auto const& b = bindings[i];
        vk_bindings.push_back(VkDescriptorSetLayoutBinding{
            .binding = b.index,
            .descriptorType = to_vk_descriptor_type(b.type),
            .descriptorCount = b.count,
            // sg has no per-stage visibility on a binding, so every stage may read it.
            // Narrowing this is a pure optimization and needs a stage set sg does not carry.
            .stageFlags = VK_SHADER_STAGE_ALL,
            .pImmutableSamplers = per_binding_immutable[i] != VK_NULL_HANDLE ? &per_binding_immutable[i] : nullptr,
        });
    }

    auto const info = VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = u32(vk_bindings.size()),
        .pBindings = vk_bindings.data(),
    };

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &info, nullptr, &layout); r != VK_SUCCESS)
    {
        for (auto sampler : immutable_samplers)
            vkDestroySampler(ctx._device, sampler, nullptr);
        return vulkan_error(r, "vkCreateDescriptorSetLayout failed");
    }

    auto owned = cc::vector<sg::binding>::create_copy_of(bindings);
    return vulkan_binding_group_layout_handle(std::make_shared<vulkan_binding_group_layout>(
        ctx, hash, cc::move(owned), layout, cc::move(immutable_samplers)));
}

vulkan_binding_group_layout::~vulkan_binding_group_layout()
{
    if (_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _layout, nullptr);
    for (auto sampler : _immutable_samplers)
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
