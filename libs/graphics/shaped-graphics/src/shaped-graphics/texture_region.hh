#pragma once

#include <shaped-graphics/backend/subresource.hh> // subresource_index, texture_aspect
#include <typed-geometry/linalg/pos.hh>           // tg::pos3i
#include <typed-geometry/linalg/vec.hh>           // tg::vec3i

/// The `texture_region` value type for host↔device texture copies (`cmd.upload.bytes_to_texture` /
/// `cmd.download.bytes_from_texture`, and their async `ctx.*` mirrors). A copy targets one subresource
/// (a `subresource_index` — mip / array-layer / aspect) and, within it, a rectangular **region**. The
/// host bytes are always **tightly packed** for the region (row pitch = region-width-in-blocks ×
/// block-bytes, no padding) — the backend adds any API-required row/placement padding when staging.

namespace sg
{
/// A box within one subresource, in texels: `offset` the near-top-left corner, `size` the extent. The copy
/// APIs may take a `cc::optional<texture_region>` — passing none copies the **whole subresource**; a given
/// region is bounds-checked, and an empty one (any `size` component <= 0) is a no-op. `offset.z` / `size.z`
/// are the 3D (W) slices — a 2D/1D subresource is one slice, so use `offset.z = 0`, `size.z = 1` (or pass no
/// region for the whole subresource). For a block-compressed format the offset and size must be block-aligned
/// (whole 4×4 blocks), except a size that runs to the subresource edge.
struct texture_region
{
    tg::pos3i offset; // corner within the subresource, in texels (zero-init = origin)
    tg::vec3i size;   // extent in texels

    /// A zero-volume box (any size component <= 0) — the copy APIs treat it as a no-op.
    [[nodiscard]] bool is_empty() const { return size[0] <= 0 || size[1] <= 0 || size[2] <= 0; }
};
} // namespace sg
