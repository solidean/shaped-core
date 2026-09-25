#include "metal_texture_view_cache.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>

namespace sg::backend::metal
{
namespace
{
/// How many array slices `desc` has in Metal's counting, which is the counting an sg array range uses.
/// A cube's six faces are slices, where `array_layers` counts whole cubes.
[[nodiscard]] isize slice_count_of(sg::texture_description const& desc)
{
    auto const layers = desc.array_layers.has_value() ? desc.array_layers.value() : 1;
    return desc.is_cube ? layers * 6 : layers;
}
} // namespace

MTL::Texture* metal_texture_view_cache::acquire(sg::raw_texture_view const& view)
{
    CC_ASSERT(view.texture != nullptr, "a texture view always names a texture");

    auto const& base = static_cast<metal_texture const&>(*view.texture);
    auto* const texture = base.texture();
    if (texture == nullptr)
        return nullptr;

    auto const& desc = base.description();
    auto const format = pixel_format_of(view.format == sg::pixel_format::undefined ? desc.format : view.format);
    auto const type = texture_type_of(view.view_dimension);

    auto const covers_whole = view.range.mip_range.start == 0 && view.range.mip_range.end == desc.mip_levels
                           && view.range.array_range.start == 0 && view.range.array_range.end == slice_count_of(desc);

    // A view of the whole texture in its own format and its own shape is the texture — minting an object for it would
    // be an allocation and a lifetime for nothing.
    // The shape has to match too: a 2D-array view of a cube covers every slice and is still a different texture type.
    if (covers_whole && format == pixel_format_of(desc.format) && type == texture->textureType())
        return texture;

    auto const key = metal_texture_view_key{
        .texture_identity = base.identity(),
        .kind = view.kind,
        .dimension = view.view_dimension,
        .format = view.format,
        .range = view.range,
        .depth_slice_range = view.depth_slice_range,
    };

    auto const minted = _views.lock(
        [&](view_map& views) -> cc::pair<MTL::Texture*, bool>
        {
            if (auto* const found = views.get_ptr(key); found != nullptr)
                return {*found, false};

            auto const scope = autorelease_scope();

            auto const mips = view.range.mip_range;
            auto const arrays = view.range.array_range;
            auto* const created = texture->newTextureView(
                format, type, NS::Range(NS::UInteger(mips.start), NS::UInteger(mips.end - mips.start)),
                NS::Range(NS::UInteger(arrays.start), NS::UInteger(arrays.end - arrays.start)));

            // A refusal is reported rather than asserted: both callers already handle null, and the combinations Metal
            // rejects — a format that cannot reinterpret this one, a slice range its type cannot hold — are a caller's
            // description rather than a backend invariant.
            if (created == nullptr)
                return {nullptr, false};

            views[key] = created;
            return {created, true};
        });

    // Outside the lock: a finalizer list is the texture's, and registering it under this cache's lock would nest two
    // locks in one order while the eviction it registers takes them in the other.
    if (minted.second)
        forget_with_texture(*view.texture, key);

    return minted.first;
}

void metal_texture_view_cache::forget_with_texture(sg::raw_texture const& texture, metal_texture_view_key key)
{
    texture.add_finalizer(
        [this, key]
        {
            MTL::Texture* doomed = nullptr;
            _views.lock(
                [&](view_map& views)
                {
                    if (auto* const found = views.get_ptr(key); found != nullptr)
                    {
                        doomed = *found;
                        (void)views.erase(key);
                    }
                });

            if (doomed != nullptr)
                doomed->release();
        });
}

isize metal_texture_view_cache::debug_entry_count()
{
    return _views.lock([](view_map& views) { return views.size(); });
}

void metal_texture_view_cache::shutdown()
{
    _views.lock(
        [](view_map& views)
        {
            for (auto&& [key, view] : views)
                if (view != nullptr)
                    view->release();
            views.clear();
        });
}
} // namespace sg::backend::metal
