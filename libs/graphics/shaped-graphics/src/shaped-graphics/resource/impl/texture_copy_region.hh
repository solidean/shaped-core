#pragma once

// Internal, not part of the public API: small helpers the public upload / download methods use to turn the optional copy region into the concrete box the backend virtuals consume.
// Kept out of the public FILE_SET on purpose, so callers outside sg never see them.

#include <shaped-graphics/resource/raw_texture.hh>    // raw_texture_handle, texture_description
#include <shaped-graphics/resource/texture_region.hh> // texture_region, subresource_index

namespace sg::impl
{
/// Asserts `sub` names a valid subresource of `texture`: mip level and array layer in range, and for now the color aspect, since depth/stencil copies are not supported yet.
/// `texture` must be non-null.
void assert_valid_subresource(raw_texture_handle const& texture, subresource_index const& sub);

/// The whole texel box of `texture`'s subresource `sub`: offset 0, and size the mip extent — depth 1 for a 2D or 1D texture, which is a single slice.
/// `texture` must be non-null and `sub.mip_level` in range; unlike assert_valid_subresource it checks neither the array layer nor the aspect.
[[nodiscard]] texture_region full_subresource_region(raw_texture_handle const& texture, subresource_index const& sub);

/// Asserts `region` lies within `texture`'s subresource `sub`: a non-negative offset, and offset+size within the mip extent.
/// So a 2D or 1D texture's depth axis stays a single slice, at z 0 with size.z 1.
void assert_texture_region_in_bounds(raw_texture_handle const& texture,
                                     subresource_index const& sub,
                                     texture_region const& region);
} // namespace sg::impl
