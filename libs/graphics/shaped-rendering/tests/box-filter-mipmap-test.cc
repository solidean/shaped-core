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
// No pixel readback: recording and submitting without a debug-layer error is what says the views and the
// dispatch shape agreed.
// The averaging itself is one line of HLSL per variant.

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
                                                                      .array_layers = 3,
                                                                      .mip_levels = 5,
                                                                      .usage = mip_usage});

    // The count is the chain's, whatever the shape: a 3D texture halving in z still has one dispatch per level,
    // and an array has one per level too — its slices ride the dispatch's z instead.
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
