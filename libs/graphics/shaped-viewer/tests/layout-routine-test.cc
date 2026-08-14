#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

// Headless: the layout routine records a whole target's draw list — border bands, placed views and a wipe — in one pass.
//
// No pixel readback.
// What this pins is everything most likely to be wrong and invisible to the plan tests: the shaders compile, one group
// layout serves all three kinds, the inline-constants block matches the cbuffer, and every pipeline variant
// (three kinds x blended / not) actually builds.
// The WARP debug layer validates the transitions for us.

using namespace cc::primitive_defines;

namespace
{
/// A texture the routine can sample — the stand-in for a view that already rendered.
[[nodiscard]] sg::texture_2d make_source(sg::context& ctx, int w, int h)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float,
         .width = w,
         .height = h,
         .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});
}

[[nodiscard]] tg::aabb2i rect_of(int x0, int y0, int x1, int y1)
{
    return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, y1));
}
} // namespace

TEST("sv - the layout routine builds its shaders and layouts")
{
    // Deliberately the narrowest case: prewarm runs init_declare and nothing else, so a failure here is the shader
    // package, the group layout or the inline-constants block rather than anything about a draw.
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    sv::layout_routine::prewarm(*ctx_h);
    CHECK(true);
}

TEST("sv - the layout routine records borders, views and a wipe in one pass")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const output_size = tg::vec2i(128, 64);
    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});

    // Two finished sources, as the plan's targets would be.
    auto const sources = cc::vector<sg::texture_2d>{make_source(ctx, 64, 64), make_source(ctx, 64, 64)};
    auto const textures = sv::plan_textures{.targets = sources, .traces = {}};

    // A frame's worth of draws by hand, in the order a plan emits them: chrome first, then content.
    auto draws = cc::vector<sv::layout_draw>();

    // The node's background: a flat fill over its whole box, which every later draw covers.
    draws.push_back(
        {.kind = sv::draw_kind::background, .dst_rect = rect_of(0, 0, 128, 128), .color = tg::vec4f(0, 0, 1, 1)});

    // A border band, which always blends so an invisible frame cannot punch a hole in what it surrounds.
    draws.push_back({.kind = sv::draw_kind::border, .dst_rect = rect_of(0, 0, 128, 2), .color = tg::vec4f(1, 0, 0, 1)});

    // An opaque view on the left, replacing whatever is under it.
    draws.push_back({.kind = sv::draw_kind::view,
                     .dst_rect = rect_of(0, 2, 64, 64),
                     .primary = {.kind = sv::draw_source_kind::target, .index = 0},
                     .sampler = sv::sampler_mode::nearest,
                     .blend = sv::layer_blend::replace});

    // A blended view over it, exercising the premultiplied-over pipeline variant.
    draws.push_back({.kind = sv::draw_kind::view,
                     .dst_rect = rect_of(0, 2, 64, 64),
                     .primary = {.kind = sv::draw_source_kind::target, .index = 1},
                     .sampler = sv::sampler_mode::linear,
                     .blend = sv::layer_blend::over,
                     .opacity = 0.5f});

    // A wipe on the right: two sources, one draw, no intermediate texture.
    draws.push_back({.kind = sv::draw_kind::wipe,
                     .dst_rect = rect_of(64, 2, 128, 64),
                     .primary = {.kind = sv::draw_source_kind::target, .index = 0},
                     .secondary = {.kind = sv::draw_source_kind::target, .index = 1},
                     .blend = sv::layer_blend::replace,
                     .post = {.kind = sv::post_process_kind::wipe, .split = 0.25f, .separator_width = 2}});

    // A cropped view, so the uv sub-rect a fit mode produces really reaches the shader.
    draws.push_back({.kind = sv::draw_kind::view,
                     .dst_rect = rect_of(96, 32, 128, 64),
                     .primary = {.kind = sv::draw_source_kind::target,
                                 .index = 0,
                                 .uv = tg::aabb2f(tg::pos2f(0.25f, 0.25f), tg::pos2f(0.75f, 0.75f))},
                     .blend = sv::layer_blend::replace});

    auto cmd = ctx.create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        sv::layout_routine::execute(scope, sv::window_id(0), draws, textures);
    }
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // Reaching here means every pipeline variant built and the whole list recorded and ran.
    CHECK(output.width() == output_size[0]);
}

TEST("sv - a degenerate rect draws nothing rather than a bad viewport")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const output = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::bgra8_unorm, .width = 32, .height = 32, .usage = sg::texture_usage::render_target});
    auto const sources = cc::vector<sg::texture_2d>{make_source(ctx, 16, 16)};

    // A collapsed cell — what a layout produces when a rect is too small for its own insets.
    auto draws = cc::vector<sv::layout_draw>();
    draws.push_back({.kind = sv::draw_kind::view,
                     .dst_rect = rect_of(10, 10, 10, 10),
                     .primary = {.kind = sv::draw_source_kind::target, .index = 0}});

    // And a draw naming a source the plan never produced, which is what a refused subtree leaves behind.
    draws.push_back({.kind = sv::draw_kind::view,
                     .dst_rect = rect_of(0, 0, 16, 16),
                     .primary = {.kind = sv::draw_source_kind::target, .index = 99}});

    auto cmd = ctx.create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        sv::layout_routine::execute(scope, sv::window_id(0), draws, {.targets = sources, .traces = {}});
    }
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    CHECK(output.width() == 32);
}
