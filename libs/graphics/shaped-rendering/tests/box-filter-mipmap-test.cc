#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <shaped-rendering/shaders.hh>

using namespace cc::primitive_defines;

// sr::box_filter_mipmap_routine over every mippable shape.
//
// A mip chain is not a 2D idea, so the routine has one shader entry point per view dimension and picks between
// them from the texture's type, compiled for the texture's own format.
// What these pin is that each shape resolves to a variant that records, and that the level accounting is
// per-shape — an array never halves its slice count, a 3D texture does halve its depth.
//
// The first test pins the level accounting and that every shape records; the two below it read the levels back
// and check exact values, since a pass that writes one slice of six submits exactly as happily as a correct one.

namespace
{
constexpr auto mip_usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst;

/// Brings every shape up for `format` before a command list opens.
///
/// **A workaround, and here to be found again.**
/// A tick drives only routines that are already REGISTERED, and `execute` is what registers one — so a caller meeting
/// a shape or a format for the first time declines that frame and draws on the next.
/// An app absorbs that; a test asserting on the first call cannot, so it names the keys up front — which couples
/// this test to a choice the code under test makes.
/// It goes away once a routine's readiness is an async, where this becomes a co_await on it;
/// issue: feat-sg-routine-readiness-async.
cc::shared_async<cc::unit> prewarm_every_variant(sg::context& ctx, sg::pixel_format format = sg::pixel_format::rgba8_unorm)
{
    for (auto v = 0; v < int(sr::mipmap_variant::count_); ++v)
        sr::box_filter_mipmap_routine::prewarm(ctx, {.shape = sr::mipmap_variant(v), .format = format});
    (void)co_await ctx.routines.idle_completion();
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - box filter mipmap generates every shape's chain",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;


    auto const tex_1d = ctx.persistent.create_texture_1d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 16, .mip_levels = 5, .usage = mip_usage});
    auto const tex_2d = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 16, .height = 16, .mip_levels = 5, .usage = mip_usage});
    auto const tex_3d = ctx.persistent.create_texture_3d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 8, .height = 8, .depth = 8, .mip_levels = 4, .usage = mip_usage});
    auto const tex_cube = ctx.persistent.create_texture_cube(
        {.format = sg::pixel_format::rgba8_unorm, .size = 16, .mip_levels = 5, .usage = mip_usage});
    auto const tex_2d_array = ctx.persistent.create_texture_2d_array({.format = sg::pixel_format::rgba8_unorm,
                                                                      .width = 16,
                                                                      .height = 16,
                                                                      .mip_levels = 5,
                                                                      .array_layers = 3,
                                                                      .usage = mip_usage});

    // The count is the chain's, whatever the shape: a 3D texture halving in z still has one dispatch per level,
    // and an array has one per level too — its slices ride an axis of the dispatch that is never halved.
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_1d) == 4);
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_2d) == 4);
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_3d) == 3);
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_cube) == 4);
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_2d_array) == 4);

    // Starting past the end is a legal no-op rather than an error, which is what lets a caller pass a texture
    // that was uploaded with its whole chain already.
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_2d, 5) == 0);
    CHECK(sr::box_filter_mipmap_routine::level_count(tex_2d, 3) == 2);

    co_await prewarm_every_variant(ctx);
    auto cmd = ctx.create_command_list();
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_1d) == sg::routine_outcome::executed);
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_2d) == sg::routine_outcome::executed);
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_3d) == sg::routine_outcome::executed);
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_cube) == sg::routine_outcome::executed);
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_2d_array) == sg::routine_outcome::executed);

    // Regenerating only the tail is the streaming case: the first levels are already good.
    CHECK(sr::box_filter_mipmap_routine::execute(*cmd, tex_2d, 3) == sg::routine_outcome::executed);
    ctx.submit_command_list(cc::move(cmd));

    ctx.advance_epoch();
    co_await ctx.idle_completion();
}

namespace
{
/// One rgba8 texel per entry, every channel set to the same value — so an average of equal texels is exactly that value again.
[[nodiscard]] cc::vector<byte> rgba8_constant(int texels, u8 value)
{
    return cc::vector<byte>::create_filled(isize(texels) * 4, byte(value));
}

/// The red channel of every texel of a tightly-packed rgba8 readback.
[[nodiscard]] cc::vector<u8> red_channel(cc::span<byte const> bytes)
{
    auto values = cc::vector<u8>();
    for (auto i = isize(0); i < bytes.size(); i += 4)
        values.push_back(u8(bytes[i]));
    return values;
}

/// Waits until `future`'s readback has landed, then takes the red channel of every texel.
///
/// Inline (`cmd.download`) rather than async: an async readback runs on the copy queue, and the mip levels this
/// reads are left in the layouts the generating dispatch put them in rather than in the common one that queue expects.
[[nodiscard]] cc::shared_async<cc::vector<u8>> read_back(sg::bytes_future const& future)
{
    auto const data = co_await future.bytes();
    co_return red_channel(data.span());
}

/// Whether every entry of `values` is `expected`, and there is at least one.
[[nodiscard]] bool all_equal(cc::span<u8 const> values, u8 expected)
{
    if (values.empty())
        return false;
    for (auto v : values)
        if (v != expected)
            return false;
    return true;
}

constexpr auto readback_usage = mip_usage | sg::texture_usage::copy_src;
} // namespace

// The shapes rather than the sizes are what this covers: a cube and a 1D array both index their slice on an axis a
// 2D-only test never exercises, and getting that axis wrong writes one slice and leaves the rest untouched.
ASYNC_INVOCABLE_TEST("sr - box filter mipmap writes every slice of every shape",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;


    // 4x4 faces and a 4-wide array, both three levels deep, so a level's own value is checkable by eye.
    auto const tex_cube = ctx.persistent.create_texture_cube(
        {.format = sg::pixel_format::rgba8_unorm, .size = 4, .mip_levels = 3, .usage = readback_usage});
    auto const tex_1d_array = ctx.persistent.create_texture_1d_array(
        {.format = sg::pixel_format::rgba8_unorm, .width = 4, .mip_levels = 3, .array_layers = 3, .usage = readback_usage});

    // A slice's own constant, and a sentinel in every level below it: a level nothing wrote reads back as the
    // sentinel, so "wrote the wrong slice" and "wrote nothing" are both failures rather than lucky zeroes.
    constexpr u8 sentinel = 255;
    auto const face_value = [](int slice) { return u8(20 * (slice + 1)); };

    co_await prewarm_every_variant(ctx);
    auto up = ctx.create_command_list();
    for (auto face = 0; face < 6; ++face)
    {
        up->upload.bytes_to_texture(tex_cube.raw(), rgba8_constant(4 * 4, face_value(face)),
                                    {.mip_level = 0, .array_layer = face});
        up->upload.bytes_to_texture(tex_cube.raw(), rgba8_constant(2 * 2, sentinel),
                                    {.mip_level = 1, .array_layer = face});
        up->upload.bytes_to_texture(tex_cube.raw(), rgba8_constant(1, sentinel), {.mip_level = 2, .array_layer = face});
    }
    for (auto slice = 0; slice < 3; ++slice)
    {
        up->upload.bytes_to_texture(tex_1d_array.raw(), rgba8_constant(4, face_value(slice)),
                                    {.mip_level = 0, .array_layer = slice});
        up->upload.bytes_to_texture(tex_1d_array.raw(), rgba8_constant(2, sentinel),
                                    {.mip_level = 1, .array_layer = slice});
        up->upload.bytes_to_texture(tex_1d_array.raw(), rgba8_constant(1, sentinel),
                                    {.mip_level = 2, .array_layer = slice});
    }
    CHECK(sr::box_filter_mipmap_routine::execute(*up, tex_cube) == sg::routine_outcome::executed);
    CHECK(sr::box_filter_mipmap_routine::execute(*up, tex_1d_array) == sg::routine_outcome::executed);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    // Every generated level of every slice, read back in one list.
    auto dl = ctx.create_command_list();
    auto futures = cc::vector<sg::bytes_future>();
    for (auto face = 0; face < 6; ++face)
        for (auto level = 1; level < 3; ++level)
            futures.push_back(dl->download.bytes_from_texture(tex_cube.raw(), {.mip_level = level, .array_layer = face}));
    for (auto slice = 0; slice < 3; ++slice)
        for (auto level = 1; level < 3; ++level)
            futures.push_back(
                dl->download.bytes_from_texture(tex_1d_array.raw(), {.mip_level = level, .array_layer = slice}));
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch();

    // Averaging equal texels reproduces them exactly, so every generated level of a face is that face's own value —
    // and a face the dispatch never covered still holds the sentinel.
    auto next = isize(0);
    for (auto face = 0; face < 6; ++face)
        for (auto level = 1; level < 3; ++level)
        {
            auto const face_texels = co_await read_back(futures[next++]);
            CHECK(all_equal(face_texels, face_value(face)));
        }

    for (auto slice = 0; slice < 3; ++slice)
        for (auto level = 1; level < 3; ++level)
        {
            auto const slice_texels = co_await read_back(futures[next++]);
            CHECK(all_equal(slice_texels, face_value(slice)));
        }
}

// An odd extent is where the halving rule stops being obvious: the second tap clamps to the level's edge rather
// than running past it, and the level below is the floor of the halved size rather than the ceiling.
ASYNC_INVOCABLE_TEST("sr - box filter mipmap halves an odd extent by averaging pairs",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;


    // 6x1 halves to 3x1 and then to 1x1, and the values are multiples of 8 so every average is an exact byte.
    auto const tex = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 6, .height = 1, .mip_levels = 3, .usage = readback_usage});

    auto base = cc::vector<byte>();
    for (auto x = 0; x < 6; ++x)
        for (auto c = 0; c < 4; ++c)
            base.push_back(byte(u8(8 * x)));

    co_await prewarm_every_variant(ctx);
    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(tex.raw(), base, {.mip_level = 0});
    CHECK(sr::box_filter_mipmap_routine::execute(*up, tex) == sg::routine_outcome::executed);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    auto dl = ctx.create_command_list();
    auto const level_1_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 1});
    auto const level_2_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 2});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch();

    // Level 1 averages each pair of the base level; level 2 has one texel left over three, so its second tap
    // clamps to the level's last texel and the third is dropped.
    auto const level_1 = co_await read_back(level_1_future);
    REQUIRE(level_1.size() == 3);
    CHECK(level_1[0] == 4);  // (0 + 8) / 2
    CHECK(level_1[1] == 20); // (16 + 24) / 2
    CHECK(level_1[2] == 36); // (32 + 40) / 2

    auto const level_2 = co_await read_back(level_2_future);
    REQUIRE(level_2.size() == 1);
    CHECK(level_2[0] == 12); // (4 + 20) / 2
}

namespace
{
/// The floats of a tightly-packed r32_float readback.
[[nodiscard]] cc::shared_async<cc::vector<f32>> read_back_floats(sg::bytes_future const& future)
{
    auto const data = co_await future.bytes();
    auto values = cc::vector<f32>::create_filled(data.size() / isize(sizeof(f32)), 0.0f);
    cc::memcpy(values.data(), data.data(), values.size() * isize(sizeof(f32)));
    co_return values;
}
} // namespace

// A format of fewer channels than the four the filter averages stores the leading ones, which is the conversion its
// shader states; distinct values pin which texels each tap reads, where a constant would pass any wiring.
ASYNC_INVOCABLE_TEST("sr - box filter mipmap averages a one-channel float format exactly",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    auto const tex = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::r32_float, .width = 4, .height = 4, .mip_levels = 3, .usage = readback_usage});

    // texel (x, y) holds x + 4y, so every average below is an exact float
    auto base = cc::vector<f32>();
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
            base.push_back(f32(x + 4 * y));
    auto bytes = cc::vector<byte>::create_filled(base.size() * isize(sizeof(f32)), byte(0));
    cc::memcpy(bytes.data(), base.data(), bytes.size());

    co_await prewarm_every_variant(ctx, sg::pixel_format::r32_float);
    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(tex.raw(), bytes, {.mip_level = 0});
    CHECK(sr::box_filter_mipmap_routine::execute(*up, tex) == sg::routine_outcome::executed);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    auto dl = ctx.create_command_list();
    auto const level_1_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 1});
    auto const level_2_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 2});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch();

    auto const level_1 = co_await read_back_floats(level_1_future);
    REQUIRE(level_1.size() == 4);
    CHECK(level_1[0] == 2.5f);  // (0 + 1 + 4 + 5) / 4
    CHECK(level_1[1] == 4.5f);  // (2 + 3 + 6 + 7) / 4
    CHECK(level_1[2] == 10.5f); // (8 + 9 + 12 + 13) / 4
    CHECK(level_1[3] == 12.5f); // (10 + 11 + 14 + 15) / 4

    auto const level_2 = co_await read_back_floats(level_2_future);
    REQUIRE(level_2.size() == 1);
    CHECK(level_2[0] == 7.5f);
}

// The one shape that halves in z: eight distinct texels, each tap reading a different one.
ASYNC_INVOCABLE_TEST("sr - box filter mipmap averages all eight texels of a 3D level",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    auto const tex = ctx.persistent.create_texture_3d({.format = sg::pixel_format::rgba8_unorm,
                                                       .width = 2,
                                                       .height = 2,
                                                       .depth = 2,
                                                       .mip_levels = 2,
                                                       .usage = readback_usage});

    // 0, 8, ..., 56: their average, 28, is an exact byte, and a tap reading one texel twice would miss it
    auto base = cc::vector<byte>();
    for (auto i = 0; i < 8; ++i)
        for (auto c = 0; c < 4; ++c)
            base.push_back(byte(u8(8 * i)));

    co_await prewarm_every_variant(ctx);
    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(tex.raw(), base, {.mip_level = 0});
    CHECK(sr::box_filter_mipmap_routine::execute(*up, tex) == sg::routine_outcome::executed);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    auto dl = ctx.create_command_list();
    auto const level_1_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 1});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch();

    auto const level_1 = co_await read_back(level_1_future);
    REQUIRE(level_1.size() == 1);
    CHECK(level_1[0] == 28);
}
