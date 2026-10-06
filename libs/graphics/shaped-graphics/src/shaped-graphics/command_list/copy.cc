#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/copy.hh>
#include <shaped-graphics/resource/impl/texture_copy_region.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

namespace sg
{
isize buffer_texture_row_pitch(pixel_format format, int width)
{
    auto const extent = format_block_extent(format);
    auto const blocks = (isize(width) + extent - 1) / extent;
    auto const bytes = blocks * format_block_size(format);
    return (bytes + buffer_texture_row_pitch_alignment - 1) / buffer_texture_row_pitch_alignment
         * buffer_texture_row_pitch_alignment;
}

void command_list_copy_scope::buffer_to_texture(buffer_texture_copy args)
{
    CC_ASSERT(args.src != nullptr, "copy source buffer is null");
    CC_ASSERT(args.dst != nullptr, "copy dest texture is null");
    impl::assert_valid_subresource(args.dst, args.subresource);
    _cmd.check_copy_outside_rendering_targets(args.dst.get(), args.subresource, "a copy into a texture");

    auto const box
        = args.region.has_value() ? args.region.value() : impl::full_subresource_region(args.dst, args.subresource);
    impl::assert_texture_region_in_bounds(args.dst, args.subresource, box);
    if (box.is_empty())
        return; // no-op

    CC_ASSERT(args.src_row_pitch_in_bytes > 0 && args.src_row_pitch_in_bytes % buffer_texture_row_pitch_alignment == 0,
              "a buffer's row pitch for a texture copy must be a positive multiple of "
              "buffer_texture_row_pitch_alignment");
    CC_ASSERT(args.src_offset_in_bytes >= 0 && args.src_offset_in_bytes % buffer_texture_offset_alignment == 0,
              "a buffer's offset for a texture copy must be a multiple of buffer_texture_offset_alignment");

    // Rows count in blocks, and the last row of the last slice need not be padded out to the pitch.
    auto const format = args.dst->description().format;
    auto const extent = format_block_extent(format);
    auto const rows = (isize(box.size[1]) + extent - 1) / extent;
    auto const row_bytes = (isize(box.size[0]) + extent - 1) / extent * format_block_size(format);
    CC_ASSERT(args.src_row_pitch_in_bytes >= row_bytes, "a buffer's row pitch is shorter than one row of the region");
    auto const needed = args.src_row_pitch_in_bytes * (rows * isize(box.size[2]) - 1) + row_bytes;
    CC_ASSERT(args.src_offset_in_bytes + needed <= args.src->size_in_bytes(), "the buffer ends before the region's "
                                                                              "last row");

    _cmd.copy_buffer_to_texture(cc::move(args.src), args.src_offset_in_bytes, args.src_row_pitch_in_bytes,
                                cc::move(args.dst), args.subresource, box);
}

void command_list_copy_scope::buffer_bytes_region(buffer_bytes_copy args)
{
    _cmd.copy_buffer_region(cc::move(args.src), cc::move(args.dst), args.src_offset_in_bytes, args.dst_offset_in_bytes,
                            args.size_in_bytes);
}
} // namespace sg
