#include "metal_texture.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>

namespace sg::backend::metal
{
metal_texture::~metal_texture()
{
    release_storage();
} // no-op if expire() already released the storage

void metal_texture::on_expired() const
{
    release_storage();
}

void metal_texture::release_storage() const
{
    if (_texture == nullptr)
        return;

    auto* const texture = _texture;
    _texture = nullptr;

    _ctx.residency().remove(texture);
    _ctx.epochs().defer([texture] { texture->release(); });
}
cc::result<metal_texture_handle> metal_context::create_metal_texture(sg::texture_description const& desc,
                                                                     sg::allocation_info const& alloc)
{
    desc.assert_valid();

    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const format = pixel_format_of(desc.format);
    if (format == MTL::PixelFormatInvalid)
        return cc::error(cc::format("texture: pixel format {} has no metal equivalent", int(desc.format)));

    auto const scope = autorelease_scope();

    auto* const descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setPixelFormat(format);
    descriptor->setTextureType(
        texture_type_of(desc.dimension, desc.array_layers.has_value(), desc.is_cube, desc.sample_count > 1));
    descriptor->setWidth(NS::UInteger(desc.width));
    descriptor->setHeight(NS::UInteger(desc.dimension == sg::texture_dimension::d1 ? 1 : desc.height));
    descriptor->setDepth(NS::UInteger(desc.dimension == sg::texture_dimension::d3 ? desc.depth : 1));
    descriptor->setMipmapLevelCount(NS::UInteger(desc.mip_levels));
    descriptor->setSampleCount(NS::UInteger(desc.sample_count));
    descriptor->setUsage(texture_usage_of(desc.usage));

    // A cube's six faces are slices in Metal's model too, and arrayLength counts cubes rather than faces — so the sg
    // layer count goes in unmultiplied either way.
    descriptor->setArrayLength(NS::UInteger(desc.array_layers.has_value() ? desc.array_layers.value() : 1));

    // Private and untracked, for the same two reasons buffers are: sg exposes no host-visible resources, and sg's own
    // access model is what emits the barriers.
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);

    MTL::Texture* texture = nullptr;
    if (alloc.is_placed())
    {
        auto const& heap = static_cast<metal_memory_heap const&>(*alloc.heap);
        texture = heap.heap()->newTexture(descriptor, NS::UInteger(alloc.offset));
    }
    else
    {
        texture = _device->newTexture(descriptor);
    }
    descriptor->release();

    if (texture == nullptr)
        return cc::error("texture: the metal device refused the allocation");

    _residency.add(texture);

    auto handle = std::make_shared<metal_texture const>(*this, desc, texture, alloc.heap);

    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring_textures.lock([&](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
                                          { v.push_back(std::weak_ptr<sg::raw_texture const>(handle)); });

    return handle;
}
} // namespace sg::backend::metal
