#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::vulkan
{
namespace
{
VkFilter to_vk_filter(sg::sampler_filter f)
{
    return f == sg::sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk_mipmap_mode(sg::sampler_filter f)
{
    return f == sg::sampler_filter::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk_address_mode(sg::sampler_address_mode m)
{
    switch (m)
    {
    case sg::sampler_address_mode::repeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case sg::sampler_address_mode::mirror_repeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case sg::sampler_address_mode::clamp_edge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case sg::sampler_address_mode::clamp_border:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case sg::sampler_address_mode::mirror_clamp_edge:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    CC_UNREACHABLE("unhandled sampler_address_mode");
}

VkBorderColor to_vk_border_color(sg::sampler_border_color c)
{
    switch (c)
    {
    case sg::sampler_border_color::transparent_black:
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case sg::sampler_border_color::opaque_black:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case sg::sampler_border_color::opaque_white:
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    CC_UNREACHABLE("unhandled sampler_border_color");
}
} // namespace

VkCompareOp to_vk_compare_op(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return VK_COMPARE_OP_NEVER;
    case sg::compare_op::less:
        return VK_COMPARE_OP_LESS;
    case sg::compare_op::equal:
        return VK_COMPARE_OP_EQUAL;
    case sg::compare_op::less_equal:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case sg::compare_op::greater:
        return VK_COMPARE_OP_GREATER;
    case sg::compare_op::not_equal:
        return VK_COMPARE_OP_NOT_EQUAL;
    case sg::compare_op::greater_equal:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case sg::compare_op::always:
        return VK_COMPARE_OP_ALWAYS;
    }
    CC_UNREACHABLE("unhandled compare_op");
}

VkSamplerCreateInfo to_vk_sampler_info(sg::sampler const& s)
{
    // max_anisotropy == 1 means anisotropy off, which Vulkan spells as a disable flag rather than a ratio of one.
    // Where it is on, the per-axis filters still apply — D3D12 has to encode anisotropy *into* the filter and thereby
    // overrides them, which is a translation this backend does not inherit.
    bool const anisotropic = s.max_anisotropy > 1;

    return VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = to_vk_filter(s.mag_filter),
        .minFilter = to_vk_filter(s.min_filter),
        .mipmapMode = to_vk_mipmap_mode(s.mip_filter),
        .addressModeU = to_vk_address_mode(s.address_u),
        .addressModeV = to_vk_address_mode(s.address_v),
        .addressModeW = to_vk_address_mode(s.address_w),
        .mipLodBias = s.mip_lod_bias,
        .anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = anisotropic ? float(s.max_anisotropy) : 1.0f,
        .compareEnable = s.compare.has_value() ? VK_TRUE : VK_FALSE,
        .compareOp = s.compare.has_value() ? to_vk_compare_op(s.compare.value()) : VK_COMPARE_OP_NEVER,
        .minLod = s.min_lod,
        .maxLod = s.max_lod,
        .borderColor = to_vk_border_color(s.border_color),
        .unnormalizedCoordinates = VK_FALSE,
    };
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
vulkan_sampler_cache::~vulkan_sampler_cache()
{
    shutdown();
}

void vulkan_sampler_cache::shutdown()
{
    // Called from context teardown before the device goes, since a VkSampler outliving its VkDevice is a leak the
    // validation layer reports at vkDestroyDevice.
    _samplers.lock(
        [&](cc::map<cc::hash128, VkSampler>& samplers)
        {
            for (auto const& [key, sampler] : samplers)
                vkDestroySampler(_ctx._device, sampler, nullptr);
            samplers.clear();
        });
}

VkSampler vulkan_sampler_cache::acquire(sg::sampler const& s)
{
    auto const key = sg::impl::sampler_hash(s);
    return _samplers.lock(
        [&](cc::map<cc::hash128, VkSampler>& samplers)
        {
            if (auto const* existing = samplers.get_ptr(key); existing != nullptr)
                return *existing;

            auto const info = to_vk_sampler_info(s);
            VkSampler sampler = VK_NULL_HANDLE;
            if (VkResult const r = vkCreateSampler(_ctx._device, &info, nullptr, &sampler); r != VK_SUCCESS)
                return VkSampler(VK_NULL_HANDLE);

            samplers[key] = sampler;
            return sampler;
        });
}
} // namespace sg::backend::vulkan
