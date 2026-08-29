#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/barrier/access_inference.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] VkImageViewType to_vk_image_view_type(sg::texture_view_dimension d)
{
    switch (d)
    {
    case sg::texture_view_dimension::tex_1d:
        return VK_IMAGE_VIEW_TYPE_1D;
    case sg::texture_view_dimension::tex_1d_array:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case sg::texture_view_dimension::tex_2d:
    case sg::texture_view_dimension::tex_2d_ms:
        // Vulkan has no separate multisampled view type; the image's sample count already says so.
        return VK_IMAGE_VIEW_TYPE_2D;
    case sg::texture_view_dimension::tex_2d_array:
    case sg::texture_view_dimension::tex_2d_ms_array:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case sg::texture_view_dimension::tex_3d:
        return VK_IMAGE_VIEW_TYPE_3D;
    case sg::texture_view_dimension::cube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case sg::texture_view_dimension::cube_array:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    CC_UNREACHABLE("unhandled texture_view_dimension");
}

/// The descriptor type a binding declares, which decides both its size and which arm of the data union to fill.
[[nodiscard]] VkDescriptorType descriptor_type_of(sg::binding_type t)
{
    switch (t)
    {
    case sg::binding_type::uniform_buffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case sg::binding_type::readonly_structured_buffer:
    case sg::binding_type::readwrite_structured_buffer:
    case sg::binding_type::readonly_raw_buffer:
    case sg::binding_type::readwrite_raw_buffer:
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
    CC_UNREACHABLE("unhandled binding_type");
}

/// The byte extent of a buffer view, whichever shape it takes.
[[nodiscard]] VkDeviceSize view_size_of(sg::raw_buffer_view const& v)
{
    return v.shape == sg::view_shape::structured ? VkDeviceSize(v.element_count * v.stride_in_bytes)
                                                 : VkDeviceSize(v.size_in_bytes);
}
} // namespace

vulkan_image_view_cache::~vulkan_image_view_cache()
{
    shutdown();
}

void vulkan_image_view_cache::shutdown()
{
    auto const destroy_all = [&](cc::mutex<cc::map<u64, VkImageView>>& guarded)
    {
        guarded.lock(
            [&](cc::map<u64, VkImageView>& views)
            {
                for (auto const& [key, view] : views)
                    if (view != VK_NULL_HANDLE)
                        vkDestroyImageView(_ctx._device, view, nullptr);
                views.clear();
            });
    };
    destroy_all(_views);
    destroy_all(_attachment_views);
}

VkImageView vulkan_image_view_cache::acquire_attachment(sg::raw_texture_handle const& texture,
                                                        sg::texture_view_dimension dimension,
                                                        sg::pixel_format format,
                                                        sg::subresource_range const& range)
{
    CC_ASSERT(texture != nullptr, "an attachment view needs a texture");
    auto const& vk_texture = static_cast<vulkan_texture const&>(*texture);

    // The identity of an attachment view is its resource plus everything that reaches vkCreateImageView.
    auto const key = cc::make_hash(texture.get(), dimension, format, range);

    return _attachment_views.lock(
        [&](cc::map<u64, VkImageView>& views) -> VkImageView
        {
            if (auto const* existing = views.get_ptr(key); existing != nullptr)
                return *existing;

            auto const info = VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = vk_texture._image,
                .viewType = to_vk_image_view_type(dimension),
                .format = to_vk_format(format),
                .subresourceRange =
                    {
                        .aspectMask = vk_aspect_mask_from(range, format),
                        .baseMipLevel = u32(range.mip_range.start),
                        .levelCount = u32(range.mip_range.end - range.mip_range.start),
                        .baseArrayLayer = u32(range.array_range.start),
                        .layerCount = u32(range.array_range.end - range.array_range.start),
                    },
            };

            VkImageView created = VK_NULL_HANDLE;
            VkResult const r = vkCreateImageView(_ctx._device, &info, nullptr, &created);
            CC_ASSERT(r == VK_SUCCESS, "vkCreateImageView failed for a rendering-scope attachment");
            views[key] = created;
            forget_with_texture(*texture, _attachment_views, key);
            return created;
        });
}

VkImageView vulkan_image_view_cache::acquire(sg::raw_texture_view const& view)
{
    CC_ASSERT(view.texture != nullptr, "a texture view with no texture has no image view");
    auto const& texture = static_cast<vulkan_texture const&>(*view.texture);
    auto const key = hash(view);

    return _views.lock(
        [&](cc::map<u64, VkImageView>& views) -> VkImageView
        {
            if (auto const* existing = views.get_ptr(key); existing != nullptr)
                return *existing;

            auto const info = VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = texture._image,
                .viewType = to_vk_image_view_type(view.view_dimension),
                // The view's own format, not the texture's: a view is allowed to reinterpret.
                .format = to_vk_format(view.format),
                .subresourceRange =
                    {
                        .aspectMask = vk_aspect_mask_from(view.range, view.format),
                        .baseMipLevel = u32(view.range.mip_range.start),
                        .levelCount = u32(view.range.mip_range.end - view.range.mip_range.start),
                        .baseArrayLayer = u32(view.range.array_range.start),
                        .layerCount = u32(view.range.array_range.end - view.range.array_range.start),
                    },
            };

            VkImageView created = VK_NULL_HANDLE;
            VkResult const r = vkCreateImageView(_ctx._device, &info, nullptr, &created);
            CC_ASSERT(r == VK_SUCCESS, "vkCreateImageView failed for a bound texture view");
            views[key] = created;
            forget_with_texture(texture, _views, key);
            return created;
        });
}

void vulkan_image_view_cache::forget_with_texture(sg::raw_texture const& texture,
                                                  cc::mutex<cc::map<u64, VkImageView>>& map,
                                                  u64 key)
{
    // Runs where the texture's own deferred release runs, so the view goes at the same moment the VkImage does.
    // `this` outlives every texture: the cache belongs to the context, and a texture cannot survive it.
    texture.add_finalizer(
        [this, &map, key]
        {
            VkImageView doomed = VK_NULL_HANDLE;
            map.lock(
                [&](cc::map<u64, VkImageView>& views)
                {
                    if (auto const* existing = views.get_ptr(key); existing != nullptr)
                    {
                        doomed = *existing;
                        views.erase(key);
                    }
                });
            if (doomed != VK_NULL_HANDLE)
                vkDestroyImageView(_ctx._device, doomed, nullptr);
        });
}

isize descriptor_size_of(vulkan_context const& ctx, sg::binding_type type)
{
    auto const& p = ctx.descriptor_buffer_properties();
    switch (type)
    {
    case sg::binding_type::uniform_buffer:
        return isize(p.uniformBufferDescriptorSize);
    case sg::binding_type::readonly_structured_buffer:
    case sg::binding_type::readwrite_structured_buffer:
    case sg::binding_type::readonly_raw_buffer:
    case sg::binding_type::readwrite_raw_buffer:
        return isize(p.storageBufferDescriptorSize);
    case sg::binding_type::readonly_texture:
        return isize(p.sampledImageDescriptorSize);
    case sg::binding_type::readwrite_texture:
        return isize(p.storageImageDescriptorSize);
    case sg::binding_type::sampler:
        return isize(p.samplerDescriptorSize);
    case sg::binding_type::acceleration_structure:
        return isize(p.accelerationStructureDescriptorSize);
    }
    CC_UNREACHABLE("unhandled binding_type in descriptor_size_of");
}

void write_view_descriptor(vulkan_context& ctx,
                           vulkan_image_view_cache& image_views,
                           sg::binding const& binding,
                           sg::raw_view const& view,
                           byte* dst)
{
    CC_ASSERT(dst != nullptr, "no destination for a descriptor write");

    auto const type = descriptor_type_of(binding.type);
    auto const size = size_t(descriptor_size_of(ctx, binding.type));

    auto info = VkDescriptorGetInfoEXT{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, .type = type};

    // Storage that must outlive the vkGetDescriptorEXT call below, since the info holds pointers into it.
    VkDescriptorAddressInfoEXT address = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    VkDescriptorImageInfo image = {};

    if (sg::is_vacant(view))
    {
        // A vacant array element still occupies a descriptor, and an unwritten one in a bound set is undefined.
        // Vulkan spells "a descriptor that reads as nothing" as an all-zero descriptor, which is defined only with
        // the nullDescriptor feature of VK_EXT_robustness2 — required at device creation for exactly this.
        //
        // The binding is the only source of shape here: the element carries no resource, which is exactly why the
        // declared type and dimension have to be on the binding rather than inferred from the view.
        for (size_t i = 0; i < size; ++i)
            dst[i] = byte(0);
        return;
    }

    if (auto const* buffer_view = sg::try_as_buffer_view(view); buffer_view != nullptr)
    {
        auto const& buffer = static_cast<vulkan_buffer const&>(*buffer_view->buffer);
        CC_ASSERT(buffer._device_address != 0, "a bound buffer has no device address");

        address.address = buffer._device_address + VkDeviceSize(buffer_view->offset_in_bytes);
        address.range = view_size_of(*buffer_view);
        address.format = VK_FORMAT_UNDEFINED; // only a texel buffer carries one, which sg has no vocabulary for yet

        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            info.data.pUniformBuffer = &address;
        else
            info.data.pStorageBuffer = &address;
    }
    else if (auto const* texture_view = sg::try_as_texture_view(view); texture_view != nullptr)
    {
        image.imageView = image_views.acquire(*texture_view);
        // The layout a descriptor is read in.
        // RADV reports descriptorBufferImageLayoutIgnored, but the spec does not guarantee that — so the honest
        // answer is the layout the access tracker actually transitions to for this view class.
        image.imageLayout = vk_layout_from(sg::shader_layout_of(texture_view->access));

        if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            info.data.pStorageImage = &image;
        else
            info.data.pSampledImage = &image;
    }
    else if (auto const* tlas_view = sg::try_as_tlas_view(view); tlas_view != nullptr)
    {
        // A descriptor names the structure by device address, the same way a TLAS instance names a BLAS.
        //
        // A null TLAS is legal and binds the null acceleration structure, which every ray misses — so address 0 is a
        // valid descriptor rather than an error, and there is nothing to keep alive for it.
        // It is the nullDescriptor feature that makes it writable at all; vkGetDescriptorEXT rejects address 0 without
        // it, which is why device creation requires VK_EXT_robustness2.
        info.data.accelerationStructure = 0;
        if (tlas_view->tlas != nullptr)
        {
            auto const& tlas = static_cast<vulkan_tlas const&>(*tlas_view->tlas);
            CC_ASSERT(tlas._address != 0, "a bound acceleration structure has no device address");
            info.data.accelerationStructure = u64(tlas._address);
        }
    }
    else
        CC_UNREACHABLE("unhandled raw_view arm in write_view_descriptor");

    ctx._descriptor_functions.get_descriptor(ctx._device, &info, size, dst);
}

void write_sampler_descriptor(vulkan_context& ctx, VkSampler sampler, byte* dst)
{
    CC_ASSERT(dst != nullptr, "no destination for a descriptor write");
    CC_ASSERT(sampler != VK_NULL_HANDLE, "a sampler descriptor needs a sampler");

    auto const info = VkDescriptorGetInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_SAMPLER,
        .data = {.pSampler = &sampler},
    };
    ctx._descriptor_functions.get_descriptor(ctx._device, &info,
                                             size_t(descriptor_size_of(ctx, sg::binding_type::sampler)), dst);
}
} // namespace sg::backend::vulkan
