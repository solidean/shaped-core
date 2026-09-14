#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh> // sg::texture_dimension
#include <shaped-graphics/types.hh>                // sg::texture_usages

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

/// The MTLTextureUsage bits `usage` implies.
///
/// Metal has no copy bits at all — every texture can be a copy source and destination — so `copy_src` and `copy_dst`
/// map to nothing, the way they are implicit on D3D12.
[[nodiscard]] MTL::TextureUsage texture_usage_of(sg::texture_usages usage);
} // namespace sg::backend::metal
