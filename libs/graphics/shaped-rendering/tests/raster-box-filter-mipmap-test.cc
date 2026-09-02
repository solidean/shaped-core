#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/raster_box_filter_mipmap_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

using namespace cc::primitive_defines;

// sr::raster_box_filter_mipmap_routine — the mip path for a format no typed UAV covers.
//
// The reason it exists is a hard rule rather than a preference: `readwrite_texture` on an sRGB format is refused,
// and D3D12 refuses it by removing the device.
// So the first thing worth pinning is that an sRGB texture reaches a generated chain at all.
//
// The second is that it reaches the RIGHT one.
// An sRGB render target converts on the way in and on the way out, so the average is taken over linear values —
// which is a different number from the average of the encoded bytes, and the test below is built so the two
// answers cannot be confused.

namespace
{
constexpr auto raster_mip_usage = sg::texture_usage::readonly_texture | sg::texture_usage::render_target
                                | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// `texels` rgba8 entries, every channel set to `value`.
[[nodiscard]] cc::vector<byte> rgba8_constant(int texels, u8 value)
{
    return cc::vector<byte>::create_filled(isize(texels) * 4, byte(value));
}

/// Four rgba8 texels: the first two `a`, the last two `b`.
[[nodiscard]] cc::vector<byte> rgba8_half_and_half(u8 a, u8 b)
{
    auto out = cc::vector<byte>();
    for (auto i = 0; i < 2; ++i)
        for (auto c = 0; c < 4; ++c)
            out.push_back(byte(a));
    for (auto i = 0; i < 2; ++i)
        for (auto c = 0; c < 4; ++c)
            out.push_back(byte(b));
    return out;
}

/// The red channel of the first texel of a tightly-packed rgba8 readback, or -1 when nothing landed.
[[nodiscard]] int first_red(sg::context& ctx, sg::bytes_future const& future)
{
    auto const data = ctx.wait_for(future);
    if (!data.has_value() || data.value().span().empty())
        return -1;
    return int(u8(data.value().span()[0]));
}
} // namespace

// Only one slib::shader_library may exist at a time — the generated package symbols are process-wide
// globals — so this cannot run beside another test that builds one.
TEST("sr - raster box filter mipmap fills an sRGB chain in linear space", exclusive("slib-shader-library"))
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

    // 2x2 down to 1x1: the whole filter in one pass, and one texel to read back.
    auto const tex_srgb = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm_srgb, .width = 2, .height = 2, .mip_levels = 2, .usage = raster_mip_usage});
    auto const tex_unorm = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 2, .height = 2, .mip_levels = 2, .usage = raster_mip_usage});

    CHECK(sr::raster_box_filter_mipmap_routine::level_count(tex_srgb) == 1);

    // Starting past the end is a legal no-op rather than an error, which is what lets a caller pass a texture
    // that was uploaded with its whole chain already.
    CHECK(sr::raster_box_filter_mipmap_routine::level_count(tex_srgb, 2) == 0);

    // Half the base black and half white, and a sentinel in the level being generated — so "wrote nothing" fails
    // rather than reading back as a plausible number.
    constexpr u8 sentinel = 77;
    auto up = ctx.create_command_list();
    for (auto const& tex : {tex_srgb, tex_unorm})
    {
        up->upload.bytes_to_texture(tex.raw(), rgba8_half_and_half(0, 255), {.mip_level = 0});
        up->upload.bytes_to_texture(tex.raw(), rgba8_constant(1, sentinel), {.mip_level = 1});
    }
    sr::raster_box_filter_mipmap_routine::execute(*up, tex_srgb);
    sr::raster_box_filter_mipmap_routine::execute(*up, tex_unorm);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch_and_wait_for_idle();

    auto dl = ctx.create_command_list();
    auto const srgb_future = dl->download.bytes_from_texture(tex_srgb.raw(), {.mip_level = 1});
    auto const unorm_future = dl->download.bytes_from_texture(tex_unorm.raw(), {.mip_level = 1});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch_and_wait_for_idle();

    auto const srgb_value = first_red(ctx, srgb_future);
    auto const unorm_value = first_red(ctx, unorm_future);

    // The two answers to "average 0 and 255", and the whole reason the sRGB one goes through a render target.
    //
    // Linear: the SRV decodes 0 and 255 to 0.0 and 1.0, the average is 0.5, and the RTV encodes that back to
    // ~187 — NOT the 127 an average of the stored bytes would give.
    // A tolerance of one step, because the encode rounds to 8 bits and the two neighbouring values are both
    // defensible; a wrong-space average is sixty steps away and no tolerance hides it.
    CHECK(srgb_value >= 186);
    CHECK(srgb_value <= 189);

    // The same shader over a format that converts nothing: the plain byte average, which is what says the
    // difference above came from the FORMAT rather than from anything the filter does.
    CHECK(unorm_value >= 126);
    CHECK(unorm_value <= 129);
}

// A chain deeper than one level, and one generated from partway down.
// The streaming case is exactly this: the file supplied the first levels and only the tail needs filling.
TEST("sr - raster box filter mipmap fills a tail of the chain", exclusive("slib-shader-library"))
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

    // 8x8 down to 1x1.
    auto const tex = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm_srgb, .width = 8, .height = 8, .mip_levels = 4, .usage = raster_mip_usage});

    CHECK(sr::raster_box_filter_mipmap_routine::level_count(tex) == 3);
    CHECK(sr::raster_box_filter_mipmap_routine::level_count(tex, 2) == 2);

    // Levels 0 and 1 supplied, 2 and 3 left as a sentinel for the routine to overwrite.
    constexpr u8 supplied = 200;
    constexpr u8 sentinel = 13;
    auto up = ctx.create_command_list();
    up->upload.bytes_to_texture(tex.raw(), rgba8_constant(8 * 8, supplied), {.mip_level = 0});
    up->upload.bytes_to_texture(tex.raw(), rgba8_constant(4 * 4, supplied), {.mip_level = 1});
    up->upload.bytes_to_texture(tex.raw(), rgba8_constant(2 * 2, sentinel), {.mip_level = 2});
    up->upload.bytes_to_texture(tex.raw(), rgba8_constant(1, sentinel), {.mip_level = 3});

    sr::raster_box_filter_mipmap_routine::execute(*up, tex, 2);
    ctx.submit_command_list(cc::move(up));
    ctx.advance_epoch_and_wait_for_idle();

    auto dl = ctx.create_command_list();
    auto const level_2 = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 2});
    auto const level_3 = dl->download.bytes_from_texture(tex.raw(), {.mip_level = 3});
    ctx.submit_command_list(cc::move(dl));
    ctx.advance_epoch_and_wait_for_idle();

    // Averaging equal texels reproduces them exactly whatever space the average is taken in, so both generated
    // levels carry the supplied value — a level chained off the one before it, not off the base.
    CHECK(first_red(ctx, level_2) == int(supplied));
    CHECK(first_red(ctx, level_3) == int(supplied));
}
