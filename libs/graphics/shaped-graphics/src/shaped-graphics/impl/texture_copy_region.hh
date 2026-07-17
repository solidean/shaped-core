#pragma once

// Internal (not part of the public API): small helpers the public upload/download methods use to turn the
// optional copy region into the concrete box the backend virtuals consume. Kept out of the public FILE_SET
// on purpose — callers outside sg never see them.

#include <shaped-graphics/raw_texture.hh>    // raw_texture_handle, texture_description
#include <shaped-graphics/texture_region.hh> // texture_region, subresource_index

namespace sg::impl
{
/// Asserts `sub` names a valid subresource of `texture`: mip level and array layer in range, and (for now)
/// the color aspect — depth/stencil copies are not supported yet. `texture` must be non-null.
void assert_valid_subresource(raw_texture_handle const& texture, subresource_index const& sub);

/// The whole texel box of `texture`'s subresource `sub`: offset 0, size = the mip extent (depth 1 for a
/// 2D/1D texture, which is a single slice). Asserts `sub` names a valid subresource.
[[nodiscard]] texture_region full_subresource_region(raw_texture_handle const& texture, subresource_index const& sub);

/// Asserts `region` lies within `texture`'s subresource `sub` — offset non-negative and offset+size within
/// the mip extent (so a 2D/1D texture's depth axis stays a single slice at z 0, size.z 1).
void assert_texture_region_in_bounds(raw_texture_handle const& texture,
                                     subresource_index const& sub,
                                     texture_region const& region);
} // namespace sg::impl
