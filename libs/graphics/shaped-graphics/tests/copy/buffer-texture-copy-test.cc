#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
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

// A mip below 4x4 is one partial block: its region is the texels the mip has, while the buffer still holds a whole block.
// It is the case a BC encoder writing a full mip chain always reaches.
ASYNC_INVOCABLE_TEST("sg - a BC block copied into a mip smaller than a block reads back unchanged",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::texture_compression_bc))
        SKIP("this device has no BC textures");

    // 16x16 with every mip: 16, 8, 4, then 2x2 and 1x1, the two that are less than a block.
    auto const texture = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::bc7_rgba_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = 16,
        .height = 16,
        .mip_levels = 5,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    // Each mip is one 16-byte block, its row at least one pitch, the second block at the next legal offset.
    auto const pitch = sg::buffer_texture_row_pitch(sg::pixel_format::bc7_rgba_unorm, 2);
    CHECK(pitch == sg::buffer_texture_row_pitch_alignment);
    auto const second = sg::buffer_texture_offset_alignment;
    auto const size = second + 16;
    auto const source = make_pattern_source(ctx, size);
    auto const bytes = pattern_bytes(size);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_buffer(source, cc::span<byte const>(bytes));
    cmd->copy.buffer_to_texture(
        {.src = source, .dst = texture, .src_row_pitch_in_bytes = pitch, .subresource = {.mip_level = 3}});
    cmd->copy.buffer_to_texture({.src = source,
                                 .dst = texture,
                                 .src_row_pitch_in_bytes = pitch,
                                 .src_offset_in_bytes = second,
                                 .subresource = {.mip_level = 4}});
    auto const mip3 = cmd->download.bytes_from_texture(texture, {.mip_level = 3});
    auto const mip4 = cmd->download.bytes_from_texture(texture, {.mip_level = 4});
    ctx->submit_command_list(cc::move(cmd));

    auto const got3 = co_await mip3.bytes();
    auto const got4 = co_await mip4.bytes();
    REQUIRE(got3.size() == 16);
    REQUIRE(got4.size() == 16);
    auto wrong = 0;
    for (auto i = isize(0); i < 16; ++i)
    {
        wrong += got3[i] != pattern(i) ? 1 : 0;
        wrong += got4[i] != pattern(second + i) ? 1 : 0;
    }
    CHECK(wrong == 0);
}

// A 3D region is rows, then depth slices of those rows, each slice `pitch × rows` after the last.
ASYNC_INVOCABLE_TEST("sg - a buffer copied into a 3D texture region lands slice by slice",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    constexpr int width = 8;
    constexpr int height = 4;
    constexpr int depth = 3;
    auto const texture = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d3,
        .width = width,
        .height = height,
        .depth = depth,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    auto const region = sg::texture_region{.offset = tg::pos3i(2, 1, 1), .size = tg::vec3i(4, 2, 2)};
    auto const pitch = sg::buffer_texture_row_pitch(sg::pixel_format::rgba8_unorm, region.size[0]);
    auto const rows = isize(region.size[1]);
    auto const size = pitch * rows * isize(region.size[2]);
    auto const source = make_pattern_source(ctx, size);
    auto const zeros = cc::vector<byte>::create_filled(width * height * depth * 4, byte(0));
    auto const bytes = pattern_bytes(size);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(zeros));
    cmd->upload.bytes_to_buffer(source, cc::span<byte const>(bytes));
    cmd->copy.buffer_to_texture({.src = source, .dst = texture, .src_row_pitch_in_bytes = pitch, .region = region});
    auto const readback = cmd->download.bytes_from_texture(texture);
    ctx->submit_command_list(cc::move(cmd));

    auto const texels = co_await readback.bytes();
    REQUIRE(texels.size() == isize(width * height * depth * 4));
    auto wrong_inside = 0;
    auto wrong_outside = 0;
    for (auto z = 0; z < depth; ++z)
        for (auto y = 0; y < height; ++y)
            for (auto x = 0; x < width; ++x)
                for (auto c = 0; c < 4; ++c)
                {
                    auto const got = texels[((isize(z) * height + y) * width + x) * 4 + c];
                    auto const inside = x >= region.offset[0] && x < region.offset[0] + region.size[0]
                                     && y >= region.offset[1] && y < region.offset[1] + region.size[1]
                                     && z >= region.offset[2] && z < region.offset[2] + region.size[2];
                    if (inside)
                    {
                        auto const at = (isize(z - region.offset[2]) * rows + isize(y - region.offset[1])) * pitch
                                      + isize(x - region.offset[0]) * 4 + c;
                        wrong_inside += got != pattern(at) ? 1 : 0;
                    }
                    else
                        wrong_outside += got != byte(0) ? 1 : 0;
                }
    CHECK(wrong_inside == 0);
    CHECK(wrong_outside == 0);
}

// One layer of an array is its own subresource: the copy writes that layer and leaves the others alone.
ASYNC_INVOCABLE_TEST("sg - a buffer copied into one array layer leaves the other layers alone",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    constexpr int width = 8;
    constexpr int height = 4;
    auto const texture = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = width,
        .height = height,
        .array_layers = 2,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });

    auto const pitch = sg::buffer_texture_row_pitch(sg::pixel_format::rgba8_unorm, width);
    auto const size = pitch * height;
    auto const source = make_pattern_source(ctx, size);
    auto const zeros = cc::vector<byte>::create_filled(width * height * 4, byte(0));
    auto const bytes = pattern_bytes(size);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(zeros), {.array_layer = 0});
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(zeros), {.array_layer = 1});
    cmd->upload.bytes_to_buffer(source, cc::span<byte const>(bytes));
    cmd->copy.buffer_to_texture(
        {.src = source, .dst = texture, .src_row_pitch_in_bytes = pitch, .subresource = {.array_layer = 1}});
    auto const layer0 = cmd->download.bytes_from_texture(texture, {.array_layer = 0});
    auto const layer1 = cmd->download.bytes_from_texture(texture, {.array_layer = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const got0 = co_await layer0.bytes();
    auto const got1 = co_await layer1.bytes();
    REQUIRE(got0.size() == isize(width * height * 4));
    REQUIRE(got1.size() == isize(width * height * 4));
    auto wrong0 = 0;
    auto wrong1 = 0;
    for (auto y = 0; y < height; ++y)
        for (auto i = 0; i < width * 4; ++i)
        {
            wrong0 += got0[isize(y) * width * 4 + i] != byte(0) ? 1 : 0;
            wrong1 += got1[isize(y) * width * 4 + i] != pattern(isize(y) * pitch + i) ? 1 : 0;
        }
    CHECK(wrong0 == 0);
    CHECK(wrong1 == 0);
}

// Where the feature is absent, a BC texture is refused at creation rather than handed to the API to fail later.
INVOCABLE_TEST("sg - a BC texture is refused where texture_compression_bc is absent", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (ctx->supports(sg::feature::texture_compression_bc))
        SKIP("this device has BC textures, so nothing is refused");

    CHECK_THROWS_AS(ctx->persistent.create_raw_texture({
                        .format = sg::pixel_format::bc1_rgba_unorm,
                        .dimension = sg::texture_dimension::d2,
                        .width = 16,
                        .height = 16,
                        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
                    }),
                    sg::allocation_exception);
}

// Writing a depth texture from bytes means a different texel layout on each backend, so every entry point refuses it up front.
// Reading one back stays legal, and is not covered here.
INVOCABLE_TEST("sg - a depth texture is refused as the destination of a copy or an upload",
               (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const depth = ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::depth32_float,
        .dimension = sg::texture_dimension::d2,
        .width = 4,
        .height = 4,
        .usage = sg::texture_usage::depth_stencil | sg::texture_usage::copy_dst,
    });
    auto const src = make_pattern_source(ctx, 256 * 4);
    auto const pixels_owner = pattern_bytes(4 * 4 * 4);
    auto const pixels = cc::span<byte const>(pixels_owner);
    auto cmd = ctx->create_command_list();

    auto const copy = [&] { cmd->copy.buffer_to_texture({.src = src, .dst = depth, .src_row_pitch_in_bytes = 256}); };
    auto const inline_upload = [&] { cmd->upload.bytes_to_texture(depth, pixels); };
    auto const async_upload = [&] { ctx->upload.bytes_to_texture(depth, cc::make_pinned_data(pixels)); };
    auto const stream_upload = [&] { (void)ctx->stream.bytes_to_texture(depth, cc::make_pinned_data(pixels)); };
    CHECK_ASSERTS(copy());
    CHECK_ASSERTS(inline_upload());
    CHECK_ASSERTS(async_upload());
    CHECK_ASSERTS(stream_upload());
    ctx->submit_command_list(cc::move(cmd));
}
