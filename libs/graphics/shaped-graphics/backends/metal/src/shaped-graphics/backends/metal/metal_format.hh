#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>    // sg::texture_dimension
#include <shaped-graphics/resource/texture_region.hh> // sg::texture_region
#include <shaped-graphics/types.hh>                   // sg::texture_usages

// Translating sg's texel formats and texture shapes into Metal's.
//
// Device-free, like the barrier translation next to it, so its tests run on any machine rather than only where a
// Metal 4 GPU exists.

namespace sg::backend::metal
{
/// The MTLPixelFormat for `format`, or `MTL::PixelFormatInvalid` for one Metal does not have.
///
/// sg's format set is already the intersection every realistic backend supports, so an invalid answer here means a
/// format was added without being mapped rather than a caller asking for something exotic.
[[nodiscard]] MTL::PixelFormat pixel_format_of(sg::pixel_format format);

/// The MTLTextureType for a shape: its dimension, whether it is an array, a cube, and whether it is multisampled.
/// Metal folds all four into one enum where sg keeps them orthogonal, so this is where the product is taken.
[[nodiscard]] MTL::TextureType texture_type_of(sg::texture_dimension dimension,
                                               bool is_array,
                                               bool is_cube,
                                               bool is_multisampled);

/// The MTLTextureType a *view* asks for, which is the view's own dimension rather than the texture's shape.
/// A one-face view of a cube is a 2D texture, and a 2D-array view of a cube is a 2D array — reusing the texture's type
/// would ask Metal for a one-slice cube, which it refuses.
[[nodiscard]] MTL::TextureType texture_type_of(sg::texture_view_dimension dimension);

/// The MTLTextureUsage bits `usage` implies.
///
/// Metal has no copy bits at all — every texture can be a copy source and destination — so `copy_src` and `copy_dst`
/// map to nothing, the way they are implicit on D3D12.
[[nodiscard]] MTL::TextureUsage texture_usage_of(sg::texture_usages usage);

} // namespace sg::backend::metal

/// How the bytes of one texture region are laid out in staging memory.
///
/// Tightly packed, which is what sg hands over and expects back: rows follow each other with no padding, and a
/// block-compressed format counts whole blocks, since a partial block at an edge still costs a full one.
struct sg::backend::metal::texture_staging_layout
{
    isize bytes_per_row = 0;
    isize bytes_per_image = 0;
    isize size_in_bytes = 0;
};

namespace sg::backend::metal
{

/// The staging layout for `region` of a texture in `format`.
/// Shared by the inline and the off-frame transfer paths, which stage identically.
[[nodiscard]] texture_staging_layout staging_layout_of(sg::pixel_format format, sg::texture_region const& region);
} // namespace sg::backend::metal
