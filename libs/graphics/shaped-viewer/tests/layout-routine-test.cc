#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <sv_shaders.hh> // sv::shaders::layout_bindings, for the group-creation test at the bottom

// Headless: the layout routine records a whole target's draw list — border bands, placed views and a wipe — in one pass.
//
// No pixel readback.
// What this pins is everything most likely to be wrong and invisible to the plan tests: the shaders compile, one group
// layout serves all three kinds, the inline-constants block matches the cbuffer, and every pipeline variant
// (three kinds x blended / not) actually builds.
// The debug layer validates the transitions for us.

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

ASYNC_INVOCABLE_TEST("sv - the layout routine builds its shaders and layouts", (sg::context_handle const& ctx_h))
{
    // Deliberately the narrowest case: prewarm registers the routine and one tick brings it up, so a failure here is
    // the shader package, the group layout or the inline-constants block rather than anything about a draw.
    // The tick is what does the work -- prewarm alone would register the routine and build nothing, and this test
    // would pass while proving nothing.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    // Prewarm names the format, because the routine is one instance per target format — and this is the case that
    // makes that worth it: an application that knows its swapchain format can have the pipelines built before the
    // first frame rather than the frame after.
    // The context is the driver's and earlier tests may already have brought the routine up, so it is evicted first.
    // Without that the tick below initializes nothing and the check proves nothing.
    sv::layout_routine::evict(*ctx_h, sg::pixel_format::bgra8_unorm);
    sv::layout_routine::prewarm(*ctx_h, sg::pixel_format::bgra8_unorm);
    auto const tick = co_await ctx_h->routines.idle_completion();
    CHECK(tick.initialized >= 1);
    CHECK(tick.is_idle());
}

ASYNC_INVOCABLE_TEST("sv - the layout routine records borders, views and a wipe in one pass",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

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

    // WORKAROUND: one instance per target format, and a tick drives only what is already registered — so the format is
    // named here exactly as the test above names it, which couples this test to the routine's parametrization.
    // Goes away once a routine's readiness is an async; see libs/graphics/shaped-graphics/docs/TODO.md, "Readiness as an async".
    sv::layout_routine::prewarm(ctx, sg::pixel_format::bgra8_unorm);
    (void)co_await ctx.routines.idle_completion();

    auto cmd = ctx.create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        CHECK(sv::layout_routine::execute(scope, sv::window_id(0), draws, textures) == sg::routine_outcome::executed);
    }
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    // Reaching here means every pipeline variant built and the whole list recorded and ran.
    CHECK(output.width() == output_size[0]);
}

ASYNC_INVOCABLE_TEST("sv - a degenerate rect draws nothing rather than a bad viewport", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

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

    // WORKAROUND: one instance per target format, and a tick drives only what is already registered — so the format is
    // named here exactly as the test above names it, which couples this test to the routine's parametrization.
    // Goes away once a routine's readiness is an async; see libs/graphics/shaped-graphics/docs/TODO.md, "Readiness as an async".
    sv::layout_routine::prewarm(ctx, sg::pixel_format::bgra8_unorm);
    (void)co_await ctx.routines.idle_completion();

    auto cmd = ctx.create_command_list();
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        CHECK(sv::layout_routine::execute(scope, sv::window_id(0), draws, {.targets = sources, .traces = {}})
              == sg::routine_outcome::executed);
    }
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();

    CHECK(output.width() == 32);
}

ASYNC_INVOCABLE_TEST("sv - a group is created against a layout whose static samplers it does not resupply",
                     (sg::context_handle const& ctx_h))
{
    // The pairing the two halves of the API have to make: `acquire_binding_group_layout<G>(runtime_samplers)`
    // bakes a sampler G left dynamic into the layout, and `create_binding_group(layout, G{...})` then gathers
    // that same sampler from G's own field.
    //
    // dx12 refuses a static sampler supplied per group outright, so without the drop this create throws --
    // which is what makes the samplers overload unusable with the create rather than merely redundant.
    auto& ctx = *ctx_h;

    using group = sv::shaders::layout_bindings;

    sg::named_sampler const runtime[]
        = {{.name = "source_sampler", .sampler = {.min_filter = sg::sampler_filter::nearest}}};
    auto const layout = ctx.cached.acquire_binding_group_layout<group>(runtime);
    REQUIRE(layout != nullptr);

    // The layout owns it now, which is the precondition the create has to respect.
    REQUIRE(layout->static_samplers().size() == 1);
    CHECK(layout->static_samplers()[0].name == "source_sampler");

    auto const source = make_source(ctx, 8, 8);
    auto cmd = ctx.create_command_list();
    auto const g = ctx.transient.create_binding_group(
        *cmd, layout,
        group{.source_0 = source.as_readonly_view(), .source_1 = source.as_readonly_view(), .source_sampler = {}});
    CHECK(g != nullptr);

    // And the layout a bare acquire gives has none, so the same create passes the gathered sampler through.
    auto const plain = ctx.cached.acquire_binding_group_layout<group>();
    REQUIRE(plain != nullptr);
    CHECK(plain->static_samplers().empty());
    CHECK(plain != layout); // the samplers are part of the identity, so these are different layouts

    auto const g2 = ctx.transient.create_binding_group(
        *cmd, plain,
        group{.source_0 = source.as_readonly_view(), .source_1 = source.as_readonly_view(), .source_sampler = {}});
    CHECK(g2 != nullptr);

    // And BOUND, which is the half a create alone cannot reach: every backend's bind_group asserts the group's
    // layout against the one the bound pipeline was built with, so a group created against a layout the pipeline
    // does not carry is caught here and nowhere earlier.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
    {
        ctx.drop_command_list(cc::move(cmd));
        SKIP("no DXC compiler to build layout.hlsl");
    }

    auto const vs = sv::shaders::layout.vertex.main_vs->acquire(ctx);
    auto const ps = sv::shaders::layout.fragment.border_ps->acquire(ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    REQUIRE(compiled_vs != nullptr);
    REQUIRE(compiled_ps != nullptr);

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled_vs->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    REQUIRE(constants_binding != nullptr);

    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {layout}, .inline_constants = *constants_binding});
    auto pipeline = ctx.cached.acquire_raster_pipeline(
        sg::raster_pipeline_description{.layout = pipeline_layout,
                                        .vertex_shader = *compiled_vs,
                                        .fragment_shader = *compiled_ps,
                                        .topology = sg::primitive_topology::triangle_list,
                                        .rasterization = {.cull = sg::cull_mode::none},
                                        .color_targets = {{.format = sg::pixel_format::rgba16_float}}});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);

    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float, .width = 8, .height = 8, .usage = sg::texture_usage::render_target});
    {
        auto scope
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        scope.bind_pipeline(*built);
        scope.bind<group>(*g);
    }
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
}
