#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// Where each mip of a tightly-packed chain sits, and how far the chain goes.
///
/// This is the layout `texture_data` documents and `cmd.upload.bytes_to_texture` consumes a subresource at a
/// time: mip 0 first, then each successive level, every row packed with no padding.
/// Block-compressed formats are not handled — sv uploads uncompressed pixels today, and a compressed chain
/// rounds its extents up to the block size rather than halving cleanly.

/// The extent of mip `level`, never below 1 on either axis.
[[nodiscard]] inline i32 mip_extent(i32 base, i32 level)
{
    auto e = base >> level;
    return e < 1 ? 1 : e;
}

/// How many levels a `width` x `height` texture has, down to 1x1 inclusive.
[[nodiscard]] inline i32 mip_count_of(i32 width, i32 height)
{
    CC_ASSERT(width > 0 && height > 0, "a texture needs a positive extent");
    auto n = i32(1);
    while (width > 1 || height > 1)
    {
        width = mip_extent(width, 1);
        height = mip_extent(height, 1);
        ++n;
    }
    return n;
}

/// The tightly-packed byte size of one mip level.
[[nodiscard]] inline isize mip_byte_size(sg::pixel_format format, i32 width, i32 height, i32 level)
{
    CC_ASSERT(!sg::is_compressed_format(format), "sv uploads uncompressed pixel formats only");
    auto const bytes_per_texel = isize(sg::format_block_size(format));
    CC_ASSERT(bytes_per_texel > 0, "an undefined pixel format has no size");
    return bytes_per_texel * isize(mip_extent(width, level)) * isize(mip_extent(height, level));
}
} // namespace sv::impl
