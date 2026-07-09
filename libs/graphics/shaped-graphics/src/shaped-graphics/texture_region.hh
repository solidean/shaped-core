#pragma once

#include <shaped-graphics/backend/subresource.hh> // subresource_index, texture_aspect
#include <typed-geometry/linalg/vec.hh>           // tg::vec3i

/// The `texture_region` value type for host↔device texture copies (`cmd.upload.bytes_to_texture` /
/// `cmd.download.bytes_from_texture`, and their async `ctx.*` mirrors). A copy targets one subresource
/// (a `subresource_index` — mip / array-layer / aspect) and, within it, a rectangular **region**. The
/// host bytes are always **tightly packed** for the region (row pitch = region-width-in-blocks ×
/// block-bytes, no padding) — the backend adds any API-required row/placement padding when staging.

namespace sg
{
/// A box within one subresource to copy, in texels: `offset` is the near-top-left corner, `size` the
/// extent. A zero `size` component runs to the subresource edge from `offset`, so a default-constructed
/// region (both zero) is the whole subresource. `z` / `size.z` are the 3D (W) slices — a 2D texture uses
/// `z = 0` and either `size.z = 1` or the default. For a block-compressed format the offset and size must
/// be block-aligned (whole 4×4 blocks), except a size that runs to the subresource edge (partial edge block).
struct texture_region
{
    tg::vec3i offset; // corner within the subresource, in texels (zero-init = origin)
    tg::vec3i size;   // extent in texels; a zero component runs to the subresource edge from `offset`
};
} // namespace sg
