#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <vulkan/vulkan.h>

// Turning a run of streamed rows into copy regions.
//
// **A streamed texture is a flat byte range, and a copy region is three-dimensional.** The async transfer paths
// chunk a texture by whole ROWS, because a row is the smallest unit a copy can place — but a row is a BLOCK row on
// a compressed format, and rows run slice-major on a 3D one.
// Vulkan's `imageOffset` and `imageExtent` are texels in every dimension and address one contiguous box, so neither
// of those maps to a row count without conversion.
//
// Getting it wrong is silent rather than loud, which is why this is its own seam with its own tests, in
// vulkan-copy-regions-test.cc.
// Handing block rows over as texel rows fills a quarter of a BC texture and leaves the rest untouched.
// Handing slice-major rows over as a height overruns a 3D texture's own height.

namespace sg::backend::vulkan
{
/// The most regions one run of rows can need.
///
/// Three: a partial first slice, a run of whole slices, and a partial last one.
/// A run starting mid-slice ends on a slice boundary or ends the chunk, so no fourth shape exists.
inline constexpr int max_copy_regions = 3;

/// Splits `row_count` rows starting at `first_row` into copy regions over `region`.
///
/// Rows are counted the way the transfer paths count them: BLOCK rows for a compressed format, and slice-major, so
/// row `rows_per_slice` is the first row of the second slice.
/// `base_offset` is where this run's bytes start in the staging buffer, and each region's offset follows from the
/// rows before it.
///
/// Returns how many entries of `out` were filled, which is never more than `max_copy_regions`.
[[nodiscard]] inline int build_texture_copy_regions(sg::texture_region const& region,
                                                    sg::pixel_format format,
                                                    isize first_row,
                                                    isize row_count,
                                                    isize row_bytes,
                                                    VkDeviceSize base_offset,
                                                    VkImageSubresourceLayers const& subresource,
                                                    cc::span<VkBufferImageCopy> out)
{
    auto const block = isize(sg::format_block_extent(format));
    auto const rows_per_slice = (isize(region.size[1]) + block - 1) / block;
    if (rows_per_slice <= 0 || row_count <= 0)
        return 0;

    auto written = 0;
    auto cur = first_row;
    auto remaining = row_count;
    auto offset = base_offset;

    while (remaining > 0 && written < int(out.size()))
    {
        auto const slice = cur / rows_per_slice;
        auto const row_in_slice = cur % rows_per_slice;

        auto consumed = isize(0);
        auto copy = VkBufferImageCopy{
            .bufferOffset = offset,
            // Zero means tightly packed to imageExtent, which is what the transfer paths hand over.
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = subresource,
            .imageOffset = {region.offset[0], region.offset[1], region.offset[2] + int(slice)},
            .imageExtent = {u32(region.size[0]), u32(region.size[1]), 1},
        };

        if (row_in_slice == 0 && remaining >= rows_per_slice)
        {
            // Whole slices in one region, which is the common case and the only one that is not row-by-row.
            auto const slices_left = isize(region.size[2]) - slice;
            auto const whole = cc::min(remaining / rows_per_slice, slices_left);
            if (whole <= 0)
                break;

            copy.imageExtent.depth = u32(whole);
            consumed = whole * rows_per_slice;
        }
        else
        {
            // A partial slice: rows within one slice, converted to texels.
            auto const rows_here = cc::min(remaining, rows_per_slice - row_in_slice);
            auto const texel_y = row_in_slice * block;

            // Clamped, because a height that is not a whole number of blocks has a last block row hanging over the
            // end — and an extent past the image is a validation error rather than a clipped copy.
            auto const texel_rows = cc::min(rows_here * block, isize(region.size[1]) - texel_y);
            if (texel_rows <= 0)
                break;

            copy.imageOffset.y = region.offset[1] + int(texel_y);
            copy.imageExtent.height = u32(texel_rows);
            consumed = rows_here;
        }

        out[written++] = copy;
        offset += VkDeviceSize(consumed * row_bytes);
        cur += consumed;
        remaining -= consumed;
    }

    return written;
}
} // namespace sg::backend::vulkan
