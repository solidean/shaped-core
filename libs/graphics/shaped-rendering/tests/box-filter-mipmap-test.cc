#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

using namespace cc::primitive_defines;

// sr::box_filter_mipmap_routine over every mippable shape.
//
// A mip chain is not a 2D idea, so the routine has one shader entry point per view dimension and picks between
// them from the texture's type.
// What these pin is that each shape resolves to a variant that records, and that the level accounting is
// per-shape — an array never halves its slice count, a 3D texture does halve its depth.
//
// The first test pins the level accounting and that every shape records; the two below it read the levels back
// and check exact values, since a pass that writes one slice of six submits exactly as happily as a correct one.

namespace
{
constexpr auto mip_usage
    = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture | sg::texture_usage::copy_dst;
} // namespace

// Only one slib::shader_library may exist at a time — the generated package symbols are process-wide
// globals — so this cannot run beside another test that builds one.
TEST("sr - box filter mipmap generates every shape's chain", exclusive("slib-shader-library"))
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // The routine acquires its shaders through the library, so without a registered package there is nothing to
    // compile and `acquire` has no library to ask.
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the mipmap shaders");

    auto shader_lib = slib::shader_library();
    shader_lib.add_compiler(cc::move(compiler.value()));
    shader_lib.add_package(sr::shader_package());

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

    auto cmd = ctx.create_command_list();
    sr::box_filter_mipmap_routine::execute(*cmd, tex_1d);
    sr::box_filter_mipmap_routine::execute(*cmd, tex_2d);
    sr::box_filter_mipmap_routine::execute(*cmd, tex_3d);
    sr::box_filter_mipmap_routine::execute(*cmd, tex_cube);
    sr::box_filter_mipmap_routine::execute(*cmd, tex_2d_array);

    // Regenerating only the tail is the streaming case: the first levels are already good.
    sr::box_filter_mipmap_routine::execute(*cmd, tex_2d, 3);
    ctx.submit_command_list(cc::move(cmd));

    ctx.advance_epoch_and_wait_for_idle();
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

/// Blocks until `future`'s readback has landed, then takes the red channel of every texel.
///
/// Inline (`cmd.download`) rather than async: an async readback runs on the copy queue, and the mip levels this
/// reads are left in the layouts the generating dispatch put them in rather than in the common one that queue expects.
[[nodiscard]] cc::vector<u8> read_back(sg::context& ctx, sg::bytes_future const& future)
{
    auto const data = ctx.wait_for(future);
    if (!data.has_value())
        return {};
    return red_channel(data.value().span());
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
TEST("sr - box filter mipmap writes every slice of every shape", exclusive("slib-shader-library"))
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the mipmap shaders");

    auto shader_lib = slib::shader_library();
    shader_lib.add_compiler(cc::move(compiler.value()));
    shader_lib.add_package(sr::shader_package());

    // 4x4 faces and a 4-wide array, both three levels deep, so a level's own value is checkable by eye.
    auto const tex_cube = ctx.persistent.create_texture_cube(
        {.format = sg::pixel_format::rgba8_unorm, .size = 4, .mip_levels = 3, .usage = readback_usage});
    auto const tex_1d_array = ctx.persistent.create_texture_1d_array(
        {.format = sg::pixel_format::rgba8_unorm, .width = 4, .mip_levels = 3, .array_layers = 3, .usage = readback_usage});

    // A slice's own constant, and a sentinel in every level below it: a level nothing wrote reads back as the
    // sentinel, so "wrote the wrong slice" and "wrote nothing" are both failures rather than lucky zeroes.
    constexpr u8 sentinel = 255;
    auto const face_value = [](int slice) { return u8(20 * (slice + 1)); };

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
    sr::box_filter_mipmap_routine::execute(*up, tex_cube);
    sr::box_filter_mipmap_routine::execute(*up, tex_1d_array);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch_and_wait_for_idle();

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
    ctx.advance_epoch_and_wait_for_idle();

    // Averaging equal texels reproduces them exactly, so every generated level of a face is that face's own value —
    // and a face the dispatch never covered still holds the sentinel.
    auto next = isize(0);
    for (auto face = 0; face < 6; ++face)
        for (auto level = 1; level < 3; ++level)
            CHECK(all_equal(read_back(ctx, futures[next++]), face_value(face)));

    for (auto slice = 0; slice < 3; ++slice)
        for (auto level = 1; level < 3; ++level)
            CHECK(all_equal(read_back(ctx, futures[next++]), face_value(slice)));
}

// An odd extent is where the halving rule stops being obvious: the second tap clamps to the level's edge rather
// than running past it, and the level below is the floor of the halved size rather than the ceiling.
TEST("sr - box filter mipmap halves an odd extent by averaging pairs", exclusive("slib-shader-library"))
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the mipmap shaders");

    auto shader_lib = slib::shader_library();
    shader_lib.add_compiler(cc::move(compiler.value()));
    shader_lib.add_package(sr::shader_package());

    // 6x1 halves to 3x1 and then to 1x1, and the values are multiples of 8 so every average is an exact byte.
    auto const tex = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 6, .height = 1, .mip_levels = 3, .usage = readback_usage});

    auto base = cc::vector<byte>();
    for (auto x = 0; x < 6; ++x)
        for (auto c = 0; c < 4; ++c)
            base.push_back(byte(u8(8 * x)));

    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(tex.raw(), base, {.mip_level = 0});
    sr::box_filter_mipmap_routine::execute(*up, tex);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch_and_wait_for_idle();

    auto dl = ctx.create_command_list();
    auto const level_1_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 1});
    auto const level_2_future = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 2});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch_and_wait_for_idle();

    // Level 1 averages each pair of the base level; level 2 has one texel left over three, so its second tap
    // clamps to the level's last texel and the third is dropped.
    auto const level_1 = read_back(ctx, level_1_future);
    REQUIRE(level_1.size() == 3);
    CHECK(level_1[0] == 4);  // (0 + 8) / 2
    CHECK(level_1[1] == 20); // (16 + 24) / 2
    CHECK(level_1[2] == 36); // (32 + 40) / 2

    auto const level_2 = read_back(ctx, level_2_future);
    REQUIRE(level_2.size() == 1);
    CHECK(level_2[0] == 12); // (4 + 20) / 2
}
