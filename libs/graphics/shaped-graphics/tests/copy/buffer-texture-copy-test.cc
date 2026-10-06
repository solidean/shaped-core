#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>

using namespace cc::primitive_defines;

// cmd.copy.buffer_to_texture over the public sg API, on every available backend.
// It is the copy a compute shader's output takes into a texture it cannot write itself, so a BC encoder's blocks are the
// case that matters; an uncompressed region pins the padding, the offset and the texels around it.

namespace
{
byte pattern(isize i)
{
    return byte((i * 37 + 11) & 0xFF);
}

sg::raw_buffer_handle make_pattern_source(sg::context_handle const& ctx, isize size)
{
    return ctx->persistent.create_raw_buffer(size, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
}

cc::vector<byte> pattern_bytes(isize size)
{
    auto out = cc::vector<byte>();
    for (auto i = isize(0); i < size; ++i)
        out.push_back(pattern(i));
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a buffer copied into a texture region lands there with its padding skipped",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    constexpr int width = 64;
    constexpr int height = 16;
    auto const texture = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = width,
        .height = height,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    // A region off the corner, whose rows of 160 bytes sit 256 apart after a 512-byte lead.
    auto const region = sg::texture_region{.offset = tg::pos3i(8, 4, 0), .size = tg::vec3i(40, 6, 1)};
    auto const pitch = sg::buffer_texture_row_pitch(sg::pixel_format::rgba8_unorm, region.size[0]);
    CHECK(pitch == 256);
    auto const offset = sg::buffer_texture_offset_alignment;
    auto const size = offset + pitch * isize(region.size[1]);
    auto const source = make_pattern_source(ctx, size);

    auto const zeros = cc::vector<byte>::create_filled(width * height * 4, byte(0));
    auto const bytes = pattern_bytes(size);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(zeros));
    cmd->upload.bytes_to_buffer(source, cc::span<byte const>(bytes));
    cmd->copy.buffer_to_texture(
        {.src = source, .dst = texture, .src_row_pitch_in_bytes = pitch, .src_offset_in_bytes = offset, .region = region});
    auto const readback = cmd->download.bytes_from_texture(texture);
    ctx->submit_command_list(cc::move(cmd));

    auto const texels = co_await readback.bytes();
    REQUIRE(texels.size() == isize(width * height * 4));

    // Inside the region each texel is its row's bytes after the lead; outside it the zeros are untouched.
    auto wrong_inside = 0;
    auto wrong_outside = 0;
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < 4; ++c)
            {
                auto const got = texels[(isize(y) * width + x) * 4 + c];
                auto const inside = x >= region.offset[0] && x < region.offset[0] + region.size[0]
                                 && y >= region.offset[1] && y < region.offset[1] + region.size[1];
                if (inside)
                {
                    auto const at = offset + isize(y - region.offset[1]) * pitch + isize(x - region.offset[0]) * 4 + c;
                    wrong_inside += got != pattern(at) ? 1 : 0;
                }
                else
                    wrong_outside += got != byte(0) ? 1 : 0;
            }
    CHECK(wrong_inside == 0);
    CHECK(wrong_outside == 0);
}

ASYNC_INVOCABLE_TEST("sg - BC7 blocks copied from a buffer read back unchanged", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::texture_compression_bc))
        SKIP("this device has no BC textures");

    // Four blocks across and two down: one block row is 64 bytes, laid 256 apart.
    constexpr int width = 16;
    constexpr int height = 8;
    auto const texture = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::bc7_rgba_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = width,
        .height = height,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    auto const pitch = sg::buffer_texture_row_pitch(sg::pixel_format::bc7_rgba_unorm, width);
    CHECK(pitch == 256);
    auto const rows = height / 4;
    auto const size = pitch * rows;
    auto const source = make_pattern_source(ctx, size);
    auto const bytes = pattern_bytes(size);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(source, cc::span<byte const>(bytes));
    cmd->copy.buffer_to_texture({.src = source, .dst = texture, .src_row_pitch_in_bytes = pitch});
    auto const readback = cmd->download.bytes_from_texture(texture);
    ctx->submit_command_list(cc::move(cmd));

    // The readback is tightly packed: each block row's 64 bytes, without the buffer's padding.
    auto const blocks = co_await readback.bytes();
    auto const row_bytes = isize(width / 4) * 16;
    REQUIRE(blocks.size() == row_bytes * rows);
    auto wrong = 0;
    for (auto r = isize(0); r < rows; ++r)
        for (auto i = isize(0); i < row_bytes; ++i)
            wrong += blocks[r * row_bytes + i] != pattern(r * pitch + i) ? 1 : 0;
    CHECK(wrong == 0);
}
