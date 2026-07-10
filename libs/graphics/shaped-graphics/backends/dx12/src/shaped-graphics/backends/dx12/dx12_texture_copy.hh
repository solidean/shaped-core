#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/texture_region.hh>

namespace sg::backend::dx12
{
/// D3D12 texture-copy alignment constants (mirroring d3d12.h, kept local so callers don't spell them).
inline constexpr cc::isize texture_row_pitch_alignment = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
inline constexpr cc::isize texture_placement_alignment = 512; // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT

/// Everything a texture upload/download job needs to stage one (sub)region through a staging buffer: the
/// resolved region, the D3D12 subresource index, the tightly-packed host layout (`row_bytes` × `rows` ×
/// `depth_slices`), and the padded staging layout (`padded_pitch` = `row_bytes` rounded up to 256, so the
/// placed footprint is legal). Rows/heights are counted in block units for block-compressed formats.
struct dx12_texture_footprint
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    cc::u32 subresource = 0; // D3D12 subresource index (mip + slice*mips + plane*mips*slices)

    int x = 0, y = 0, z = 0;              // destination offset within the subresource, in texels
    int width = 0, height = 0, depth = 0; // region extent, in texels (block-aligned for BC)
    int sub_width = 0, sub_height = 0;    // the subresource's (mip) extent — for BC edge-block rounding on readback

    cc::isize row_bytes = 0; // tightly-packed bytes per row (a block-row for BC): blocks_wide × block_bytes
    int rows = 0;            // number of rows (block-rows for BC) per depth slice
    int depth_slices = 0;    // number of depth (W) slices in the region
    int block_extent = 1;    // texel height of one row: 1 (uncompressed) or 4 (block-compressed)

    cc::isize padded_pitch = 0; // row_bytes rounded up to texture_row_pitch_alignment (256)

    [[nodiscard]] cc::isize tight_size() const { return row_bytes * cc::isize(rows) * cc::isize(depth_slices); }
    [[nodiscard]] cc::isize staged_size() const { return padded_pitch * cc::isize(rows) * cc::isize(depth_slices); }
};

/// One CopyTextureRegion's worth of a (sub)region, carved out while packing its rows into staging windows.
/// Either a run of whole depth slices (`slice_count` >= 1, all `fp.rows` rows each) or a partial run of
/// block-rows within a single slice (`slice_count` == 1). A copy that fits its window is one chunk; one
/// that straddles a ring seam / window edge splits into several (up to four for 3D). `staging_rows` counts
/// the padded rows it occupies.
struct dx12_texture_copy_chunk
{
    int slice_start = 0;
    int slice_count = 1;
    int row_start = 0; // first block-row within each slice (0 for a whole-slice run)
    int row_count = 0; // block-rows per slice

    [[nodiscard]] cc::isize staging_rows() const { return cc::isize(slice_count) * cc::isize(row_count); }
};

/// Picks the largest copy chunk that fits `max_staging_rows` staging rows, starting at cursor
/// (`slice`, `row`). Prefers whole depth slices when the cursor sits on a slice boundary and at least one
/// full slice fits; otherwise a partial block-row run within the current slice. `max_staging_rows` must be
/// >= 1 and `slice` < `fp.depth_slices`.
[[nodiscard]] dx12_texture_copy_chunk next_texture_copy_chunk(dx12_texture_footprint const& fp,
                                                              int slice,
                                                              int row,
                                                              cc::isize max_staging_rows);

/// Computes the copy footprint of a concrete (subresource, region) against `desc`. The region is the box
/// already resolved + bounds-checked by the sg layer, so it is in-bounds
/// and non-empty here. Asserts on a non-color aspect (depth/stencil transfer is not supported yet) or a
/// region that is not block-aligned for a block-compressed format.
[[nodiscard]] dx12_texture_footprint compute_texture_footprint(sg::texture_description const& desc,
                                                               sg::subresource_index const& subresource,
                                                               sg::texture_region const& region);
} // namespace sg::backend::dx12
