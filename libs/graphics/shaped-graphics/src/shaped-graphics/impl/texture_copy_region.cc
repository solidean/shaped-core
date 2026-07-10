#include <shaped-graphics/impl/texture_copy_region.hh>

#include <clean-core/common/assertf.hh>

namespace sg::impl
{
namespace
{
// Extent of a mip level along one axis: the base extent halved per level, floored at 1.
[[nodiscard]] int mip_extent(int base, int mip)
{
    int const e = base >> mip;
    return e < 1 ? 1 : e;
}
} // namespace

void assert_valid_subresource(raw_texture_handle const& texture, subresource_index const& sub)
{
    CC_ASSERT(texture != nullptr, "texture is null");
    texture_description const& desc = texture->description();
    CC_ASSERTF(sub.mip_level >= 0 && sub.mip_level < desc.mip_levels, "mip level {} out of range [0, {})", sub.mip_level,
               desc.mip_levels);
    int const layers = desc.array_layers.value_or(1) * (desc.is_cube ? 6 : 1);
    CC_ASSERTF(sub.array_layer >= 0 && sub.array_layer < layers, "array layer {} out of range [0, {})", sub.array_layer, layers);
    CC_ASSERT(sub.aspect == texture_aspect::color, "only the color aspect is supported for texture copies yet");
}

texture_region full_subresource_region(raw_texture_handle const& texture, subresource_index const& sub)
{
    CC_ASSERT(texture != nullptr, "texture is null");
    texture_description const& desc = texture->description();
    CC_ASSERTF(sub.mip_level >= 0 && sub.mip_level < desc.mip_levels, "mip level {} out of range [0, {})", sub.mip_level,
               desc.mip_levels);

    int const w = mip_extent(desc.width, sub.mip_level);
    int const h = desc.dimension == texture_dimension::d1 ? 1 : mip_extent(desc.height, sub.mip_level);
    int const d = desc.dimension == texture_dimension::d3 ? mip_extent(desc.depth, sub.mip_level) : 1;
    return texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(w, h, d)};
}

void assert_texture_region_in_bounds(raw_texture_handle const& texture, subresource_index const& sub, texture_region const& region)
{
    texture_region const full = full_subresource_region(texture, sub);
    CC_ASSERTF(region.offset[0] >= 0 && region.offset[1] >= 0 && region.offset[2] >= 0,
               "texture copy region offset must be non-negative, got ({}, {}, {})", region.offset[0], region.offset[1],
               region.offset[2]);
    CC_ASSERTF(region.offset[0] + region.size[0] <= full.size[0] && region.offset[1] + region.size[1] <= full.size[1]
                   && region.offset[2] + region.size[2] <= full.size[2],
               "texture copy region offset ({}, {}, {}) + size ({}, {}, {}) exceeds subresource extent ({}, {}, {})",
               region.offset[0], region.offset[1], region.offset[2], region.size[0], region.size[1], region.size[2],
               full.size[0], full.size[1], full.size[2]);
}
} // namespace sg::impl
