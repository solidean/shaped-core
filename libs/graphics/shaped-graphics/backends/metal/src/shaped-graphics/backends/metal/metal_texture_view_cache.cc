#include "metal_texture_view_cache.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>

namespace sg::backend::metal
{
MTL::Texture* metal_texture_view_cache::acquire(sg::raw_texture_view const& view)
{
    CC_ASSERT(view.texture != nullptr, "a texture view always names a texture");

    auto const& base = static_cast<metal_texture const&>(*view.texture);
    auto* const texture = base.texture();
    if (texture == nullptr)
        return nullptr;

    auto const& desc = base.description();
    auto const format = pixel_format_of(view.format == sg::pixel_format::undefined ? desc.format : view.format);

    auto const layers = desc.array_layers.has_value() ? desc.array_layers.value() : 1;
    auto const covers_whole = view.range.mip_range.start == 0 && view.range.mip_range.end == desc.mip_levels
                           && view.range.array_range.start == 0 && view.range.array_range.end == layers;

    // A view of the whole texture in its own format is the texture — minting an object for it would be an allocation
    // and a lifetime for nothing.
    if (covers_whole && format == pixel_format_of(desc.format))
        return texture;

    auto const key = hash(view);

    return _views.lock(
        [&](cc::map<u64, MTL::Texture*>& views) -> MTL::Texture*
        {
            if (auto* const found = views.get_ptr(key); found != nullptr)
                return *found;

            auto const scope = autorelease_scope();

            auto const type
                = texture_type_of(desc.dimension, desc.array_layers.has_value(), desc.is_cube, desc.sample_count > 1);
            auto const mips = view.range.mip_range;
            auto const arrays = view.range.array_range;
            auto* const minted = texture->newTextureView(
                format, type, NS::Range(NS::UInteger(mips.start), NS::UInteger(mips.end - mips.start)),
                NS::Range(NS::UInteger(arrays.start), NS::UInteger(arrays.end - arrays.start)));

            CC_ASSERT(minted != nullptr, "metal refused a texture view");
            views[key] = minted;
            return minted;
        });
}

void metal_texture_view_cache::shutdown()
{
    _views.lock(
        [](cc::map<u64, MTL::Texture*>& views)
        {
            for (auto&& [key, view] : views)
                if (view != nullptr)
                    view->release();
            views.clear();
        });
}
} // namespace sg::backend::metal
