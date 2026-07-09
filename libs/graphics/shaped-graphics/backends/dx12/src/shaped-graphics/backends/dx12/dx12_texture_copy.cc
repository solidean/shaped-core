#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::min
#include <shaped-graphics/backends/dx12/dx12_format.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] cc::isize align_up(cc::isize v, cc::isize a)
{
    return (v + a - 1) / a * a;
}

// Extent of a mip level along one axis: the base extent halved per level, floored at 1.
[[nodiscard]] int mip_extent(int base, int mip)
{
    int const e = base >> mip;
    return e < 1 ? 1 : e;
}
} // namespace

dx12_texture_footprint compute_texture_footprint(sg::texture_description const& desc,
                                                 sg::subresource_index sub,
                                                 sg::texture_region region)
{
    CC_ASSERT(sub.aspect == sg::texture_aspect::color, "only the color aspect is supported for texture copies yet");
    CC_ASSERT(sub.mip_level >= 0 && sub.mip_level < desc.mip_levels, "mip level out of range");

    // The array axis size D3D12 addresses subresources against (a cube is 6 slices per cube).
    int const slices = desc.array_layers.value_or(1) * (desc.is_cube ? 6 : 1);
    CC_ASSERT(sub.array_layer >= 0 && sub.array_layer < slices, "array layer out of range");

    int const sub_w = mip_extent(desc.width, sub.mip_level);
    int const sub_h = desc.dimension == sg::texture_dimension::d1 ? 1 : mip_extent(desc.height, sub.mip_level);
    int const sub_d = desc.dimension == sg::texture_dimension::d3 ? mip_extent(desc.depth, sub.mip_level) : 1;

    // tg::vec is subscript-accessed ([0]/[1]/[2] = x/y/z); pull the axes out for the region math below.
    int const ox = region.offset[0], oy = region.offset[1], oz = region.offset[2];

    // A default (zero-size) region resolves to the whole subresource from the offset.
    int const w = region.size[0] > 0 ? region.size[0] : sub_w - ox;
    int const h = region.size[1] > 0 ? region.size[1] : sub_h - oy;
    int const d = region.size[2] > 0 ? region.size[2] : sub_d - oz;
    CC_ASSERT(ox >= 0 && oy >= 0 && oz >= 0, "region offset must be non-negative");
    CC_ASSERT(w > 0 && h > 0 && d > 0, "region must be non-empty");
    CC_ASSERT(ox + w <= sub_w && oy + h <= sub_h && oz + d <= sub_d, "region out of bounds");

    int const block = sg::format_block_extent(desc.format); // 1 (uncompressed) or 4 (block-compressed)
    cc::isize const block_bytes = sg::format_block_size(desc.format);
    CC_ASSERT(block_bytes > 0, "texture format has no defined block size");
    if (block > 1)
    {
        CC_ASSERT(ox % block == 0 && oy % block == 0, "block-compressed copy offset must be block-aligned");
        // The size must be block-aligned, except a region that runs to the subresource edge (partial edge block).
        CC_ASSERT((w % block == 0 || ox + w == sub_w) && (h % block == 0 || oy + h == sub_h),
                  "block-compressed copy size must be block-aligned (except at the subresource edge)");
    }

    dx12_texture_footprint fp;
    fp.format = to_dxgi_format(desc.format);
    // D3D12 subresource index = mip + slice*mipLevels (+ plane*mipLevels*slices; plane 0 for color).
    fp.subresource = cc::u32(sub.mip_level) + cc::u32(sub.array_layer) * cc::u32(desc.mip_levels);
    fp.x = ox;
    fp.y = oy;
    fp.z = oz;
    fp.width = w;
    fp.height = h;
    fp.depth = d;
    fp.sub_width = sub_w;
    fp.sub_height = sub_h;

    int const blocks_wide = (w + block - 1) / block;
    fp.rows = (h + block - 1) / block;
    fp.depth_slices = d;
    fp.block_extent = block;
    fp.row_bytes = cc::isize(blocks_wide) * block_bytes;
    fp.padded_pitch = align_up(fp.row_bytes, texture_row_pitch_alignment);
    return fp;
}

dx12_texture_copy_chunk next_texture_copy_chunk(dx12_texture_footprint const& fp,
                                                int slice,
                                                int row,
                                                cc::isize max_staging_rows)
{
    CC_ASSERT(max_staging_rows >= 1, "a texture copy chunk needs room for at least one padded row");
    CC_ASSERT(slice >= 0 && slice < fp.depth_slices, "chunk cursor slice out of range");
    CC_ASSERT(row >= 0 && row < fp.rows, "chunk cursor row out of range");

    dx12_texture_copy_chunk chunk;
    chunk.slice_start = slice;
    if (row == 0 && max_staging_rows >= cc::isize(fp.rows))
    {
        // At a slice boundary with room for at least one full slice: batch as many whole slices as fit, so a
        // copy that fits its window is a single CopyTextureRegion (a 3D copy stays one chunk when it all fits).
        int const slices = int(cc::min(max_staging_rows / cc::isize(fp.rows), cc::isize(fp.depth_slices - slice)));
        chunk.slice_count = slices;
        chunk.row_start = 0;
        chunk.row_count = fp.rows;
    }
    else
    {
        // Mid-slice, or a slice that does not fit whole: a partial block-row run within the current slice.
        chunk.slice_count = 1;
        chunk.row_start = row;
        chunk.row_count = int(cc::min(max_staging_rows, cc::isize(fp.rows - row)));
    }
    return chunk;
}
} // namespace sg::backend::dx12
