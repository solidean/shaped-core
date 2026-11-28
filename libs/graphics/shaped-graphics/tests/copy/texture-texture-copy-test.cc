#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/raw_texture.hh>

using namespace cc::primitive_defines;

// cmd.copy.texture_to_texture over the public sg API, on every available backend:
// a box between two textures, between two mips of one texture, and of whole blocks of a compressed format.

namespace
{
byte pattern(isize i)
{
    return byte((i * 37 + 11) & 0xFF);
}

cc::vector<byte> pattern_bytes(isize size)
{
    auto out = cc::vector<byte>();
    for (auto i = isize(0); i < size; ++i)
        out.push_back(pattern(i));
    return out;
}

sg::raw_texture_handle make_rgba8(sg::context_handle const& ctx, int width, int height, int mips = 1)
{
    return ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = width,
        .height = height,
        .mip_levels = mips,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a box of one texture copied into another lands at its offset, the texels around it "
                     "untouched",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    constexpr int width = 32;
    constexpr int height = 16;
    auto const src = make_rgba8(ctx, width, height);
    auto const dst = make_rgba8(ctx, width, height);
    auto const texels = pattern_bytes(width * height * 4);
    auto const zeros = cc::vector<byte>::create_filled(width * height * 4, byte(0));

    // A box off the source's corner, landing off the destination's other corner.
    auto const box = sg::texture_region{.offset = tg::pos3i(3, 2, 0), .size = tg::vec3i(20, 9, 1)};
    auto const to = tg::pos3i(10, 5, 0);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src, cc::span<byte const>(texels));
    cmd->upload.bytes_to_texture(dst, cc::span<byte const>(zeros));
    cmd->copy.texture_to_texture({.src = src, .dst = dst, .region = box, .dst_offset = to});
    auto const readback = cmd->download.bytes_from_texture(dst);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await readback.bytes();
    REQUIRE(got.size() == isize(width * height * 4));
    auto wrong_inside = 0;
    auto wrong_outside = 0;
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            for (auto c = 0; c < 4; ++c)
            {
                auto const at = (isize(y) * width + x) * 4 + c;
                auto const inside = x >= to[0] && x < to[0] + box.size[0] && y >= to[1] && y < to[1] + box.size[1];
                if (inside)
                {
                    auto const from = (isize(y - to[1] + box.offset[1]) * width + (x - to[0] + box.offset[0])) * 4 + c;
                    wrong_inside += got[at] != pattern(from) ? 1 : 0;
                }
                else
                    wrong_outside += got[at] != byte(0) ? 1 : 0;
            }
    CHECK(wrong_inside == 0);
    CHECK(wrong_outside == 0);
}

ASYNC_INVOCABLE_TEST("sg - a mip copied into another mip of the same texture reads back unchanged",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // 16x16 with three mips: mip 2's 4x4 copied into the corner of mip 1's 8x8.
    auto const texture = make_rgba8(ctx, 16, 16, 3);
    auto const mip1 = cc::vector<byte>::create_filled(8 * 8 * 4, byte(0));
    auto const mip2 = pattern_bytes(4 * 4 * 4);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(mip1), {.mip_level = 1});
    cmd->upload.bytes_to_texture(texture, cc::span<byte const>(mip2), {.mip_level = 2});
    cmd->copy.texture_to_texture(
        {.src = texture, .dst = texture, .src_subresource = {.mip_level = 2}, .dst_subresource = {.mip_level = 1}});
    auto const readback = cmd->download.bytes_from_texture(texture, {.mip_level = 1});
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await readback.bytes();
    REQUIRE(got.size() == 8 * 8 * 4);
    auto wrong = 0;
    for (auto y = 0; y < 8; ++y)
        for (auto x = 0; x < 8; ++x)
            for (auto c = 0; c < 4; ++c)
            {
                auto const expected = x < 4 && y < 4 ? pattern((isize(y) * 4 + x) * 4 + c) : byte(0);
                wrong += got[(isize(y) * 8 + x) * 4 + c] != expected ? 1 : 0;
            }
    CHECK(wrong == 0);
}

ASYNC_INVOCABLE_TEST("sg - BC7 blocks copied between textures read back unchanged", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::texture_compression_bc))
        SKIP("this device has no BC textures");

    // Four blocks across and two down; the second block row of the source lands as the first of the destination.
    auto const make = [&]
    {
        return ctx->persistent.create_raw_texture({
            .format = sg::pixel_format::bc7_rgba_unorm,
            .dimension = sg::texture_dimension::d2,
            .width = 16,
            .height = 8,
            .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
        });
    };
    auto const src = make();
    auto const dst = make();
    auto const blocks = pattern_bytes(4 * 2 * 16);
    auto const zeros = cc::vector<byte>::create_filled(4 * 2 * 16, byte(0));

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src, cc::span<byte const>(blocks));
    cmd->upload.bytes_to_texture(dst, cc::span<byte const>(zeros));
    cmd->copy.texture_to_texture({.src = src,
                                  .dst = dst,
                                  .region = sg::texture_region{.offset = tg::pos3i(0, 4, 0), .size = tg::vec3i(16, 4, 1)},
                                  .dst_offset = tg::pos3i(0, 0, 0)});
    auto const readback = cmd->download.bytes_from_texture(dst);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await readback.bytes();
    REQUIRE(got.size() == 4 * 2 * 16);
    auto wrong = 0;
    for (auto i = isize(0); i < 4 * 16; ++i)
    {
        wrong += got[i] != pattern(4 * 16 + i) ? 1 : 0;
        wrong += got[4 * 16 + i] != byte(0) ? 1 : 0;
    }
    CHECK(wrong == 0);
}
