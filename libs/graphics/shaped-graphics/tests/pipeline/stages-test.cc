#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// stages.sgl's geometry and tessellation stages, drawn where the device has them.
// A geometry stage the device lacks is drawn as its polyfill where the backend draws one, to the same images (CHK-525).
// Every probe is a pixel centre well inside or well outside each triangle, so no rasterization tie-break decides it.

namespace
{
constexpr int size = 16;

/// The clip-space centre of pixel (x, y) of the `size` × `size` target.
tg::vec2f centre_of(int x, int y)
{
    return tg::vec2f(-1.0f + (2.0f * float(x) + 1.0f) / float(size), 1.0f - (2.0f * float(y) + 1.0f) / float(size));
}

/// Where `p` sits against triangle `a, b, c`: 1 inside, -1 outside, 0 within a pixel's tenth of an edge.
int side_of(tg::vec2f p, tg::vec2f a, tg::vec2f b, tg::vec2f c)
{
    auto const edge = [&](tg::vec2f from, tg::vec2f to)
    {
        auto const d = to - from;
        return (d[0] * (p[1] - from[1]) - d[1] * (p[0] - from[0])) / d.length();
    };
    auto const e0 = edge(a, b);
    auto const e1 = edge(b, c);
    auto const e2 = edge(c, a);
    auto const margin = 0.2f / float(size);
    auto const all_positive = e0 > margin && e1 > margin && e2 > margin;
    auto const all_negative = e0 < -margin && e1 < -margin && e2 < -margin;
    if (all_positive || all_negative)
        return 1;
    auto const near = [&](float e) { return e > -margin && e < margin; };
    if (near(e0) || near(e1) || near(e2))
        return 0;
    return -1;
}

sg_test::offscreen stage_target()
{
    return {.width = size,
            .height = size,
            .colors = {sg::pixel_format::rgba32_float},
            .target_set = shaders::stages::stage_target::name,
            .clear_color = tg::vec4f(-1, -1, -1, -1)};
}

bool drawn(sg_test::offscreen_pixels const& pixels, int x, int y)
{
    return pixels[0].rgba_float(x, y)[3] == 1.0f;
}

/// Why `ctx` draws no geometry stage at all, or nothing where it draws one, natively or as its polyfill.
cc::optional<cc::string_view> no_geometry_stage(sg::context const& ctx)
{
    if (!sg_test::shaders_reach(ctx))
        return cc::string_view("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx.supports(sg::feature::geometry_shader) && !ctx.draws_geometry_polyfill())
        return cc::string_view("this device has no geometry stage, and its backend draws no polyfilled one");
    return {};
}

/// The routes a polyfilled geometry stage takes on `ctx`, each drawn to the same images: one pass, the native stage's,
/// where the device has a geometry stage; the compute route, and the mesh route where the device has mesh shaders.
cc::vector<sg::geometry_polyfill_route> geometry_routes_of(sg::context const& ctx)
{
    auto routes = cc::vector<sg::geometry_polyfill_route>{sg::geometry_polyfill_route::compute};
    if (!ctx.supports(sg::feature::geometry_shader) && ctx.supports(sg::feature::mesh_shader)
        && ctx.draws_geometry_mesh_route())
        routes.push_back(sg::geometry_polyfill_route::mesh);
    return routes;
}

/// Draws a polyfilled geometry stage by `route` while it lives, and restores the route the context took before.
struct geometry_routing
{
    sg::context& ctx;
    sg::geometry_polyfill_route before;

    geometry_routing(sg::context& c, sg::geometry_polyfill_route route) : ctx(c), before(c.geometry_polyfill_route())
    {
        ctx.set_geometry_polyfill_route(route);
    }
    ~geometry_routing() { ctx.set_geometry_polyfill_route(before); }
    geometry_routing(geometry_routing const&) = delete;
    geometry_routing& operator=(geometry_routing const&) = delete;
};

/// Whether `pipeline` was built by `route` on `ctx`, which a test checks so a cached pipeline of the other route is caught.
bool is_built_by(sg::context const& ctx, sg::raster_pipeline const& pipeline, sg::geometry_polyfill_route route)
{
    if (ctx.supports(sg::feature::geometry_shader))
        return true;
    return (pipeline.geometry_mesh_corners() > 0) == (route == sg::geometry_polyfill_route::mesh);
}

/// Polyfills a geometry stage the device lacks while it lives, and restores what the context did before.
struct geometry_polyfilling
{
    sg::context& ctx;
    sg::polyfill_use before;

    explicit geometry_polyfilling(sg::context& c) : ctx(c), before(c.geometry_polyfill())
    {
        ctx.set_geometry_polyfill(sg::polyfill_use::where_missing);
    }
    ~geometry_polyfilling() { ctx.set_geometry_polyfill(before); }
    geometry_polyfilling(geometry_polyfilling const&) = delete;
    geometry_polyfilling& operator=(geometry_polyfilling const&) = delete;
};

/// Why `ctx` draws no tessellation at all, or nothing where it draws it, natively or as its polyfill.
cc::optional<cc::string_view> no_tessellation(sg::context const& ctx)
{
    if (!sg_test::shaders_reach(ctx))
        return cc::string_view("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx.supports(sg::feature::tessellation_shader) && !ctx.draws_tessellation_polyfill())
        return cc::string_view("this device has no tessellation stages, and its backend draws no polyfilled ones");
    return {};
}

/// Polyfills tessellation stages the device lacks while it lives, and restores what the context did before.
struct tessellation_polyfilling
{
    sg::context& ctx;
    sg::polyfill_use before;

    explicit tessellation_polyfilling(sg::context& c) : ctx(c), before(c.tessellation_polyfill())
    {
        ctx.set_tessellation_polyfill(sg::polyfill_use::where_missing);
    }
    ~tessellation_polyfilling() { ctx.set_tessellation_polyfill(before); }
    tessellation_polyfilling(tessellation_polyfilling const&) = delete;
    tessellation_polyfilling& operator=(tessellation_polyfilling const&) = delete;
};
} // namespace

ASYNC_INVOCABLE_TEST("sg - a tessellated triangle patch covers its triangle, reports its domain, and winds as its "
                     "control stage says",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_tessellation(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = tessellation_polyfilling(*ctx);

    // The patch is the lower-left half of the target, its corners counter-clockwise in clip space.
    // The evaluation stage writes its domain location as the color: each pixel centre's barycentric coordinates against the three corners.
    auto const p0 = tg::vec2f(-1, -1);
    auto const p1 = tg::vec2f(1, -1);
    auto const p2 = tg::vec2f(-1, 1);
    shaders::stages::stage_corner const corners[] = {{.position = p0}, {.position = p1}, {.position = p2}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);

    auto const draw_with = [&](auto const& declared) -> cc::shared_async<sg_test::offscreen_pixels>
    {
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(declared);
        co_return co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                   });
    };

    // Culling back faces with counter-clockwise fronts, the patch whose control stage says counter-clockwise is drawn whole and the clockwise one culled.
    // So the winding a control stage names is the patch's, as the evaluation below weighs its corners (CHK-304).
    auto const ccw = co_await draw_with(shaders::stages::tessellated_ccw);
    auto const cw = co_await draw_with(shaders::stages::tessellated_cw);
    auto const culled = co_await draw_with(shaders::stages::tessellated_culled);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const c = centre_of(x, y);
            auto const side = side_of(c, p0, p1, p2);
            if (side == 0)
                continue;
            auto const where = cc::format("pixel ({}, {})", x, y);
            CHECK(drawn(ccw, x, y) == (side > 0)).context(where);
            CHECK(!drawn(cw, x, y)).context(where);
            CHECK(!drawn(culled, x, y)).context(where); // an edge factor of zero culls the patch
            if (side > 0)
            {
                auto const v = (c[0] + 1.0f) / 2.0f;
                auto const w = (c[1] + 1.0f) / 2.0f;
                auto const got = ccw[0].rgba_float(x, y);
                auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
                CHECK((near(got[0], 1.0f - v - w) && near(got[1], v) && near(got[2], w)))
                    .context(cc::format("{}: domain ({}, {}, {})", where, got[0], got[1], got[2]));
            }
        }
}

ASYNC_INVOCABLE_TEST("sg - a control stage's max_factor cuts a patch as that factor does, and its evaluation stage "
                     "reads the capped factors (CHK-304)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_tessellation(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = tessellation_polyfilling(*ctx);

    auto const p0 = tg::vec2f(-1, -1);
    auto const p1 = tg::vec2f(1, -1);
    auto const p2 = tg::vec2f(-1, 1);
    shaders::stages::stage_corner const corners[] = {{.position = p0}, {.position = p1}, {.position = p2}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const draw_with = [&](auto const& declared) -> cc::shared_async<sg_test::offscreen_pixels>
    {
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(declared);
        co_return co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                   });
    };

    // Squared domain locations interpolate linearly across one triangle, and bend where the patch is cut finer.
    auto const capped = co_await draw_with(shaders::stages::tessellated_capped);
    auto const finer = co_await draw_with(shaders::stages::tessellated_squared);
    auto const near = [](float a, float b, float tolerance) { return a - b < tolerance && b - a < tolerance; };
    auto bent = 0;
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const c = centre_of(x, y);
            if (side_of(c, p0, p1, p2) <= 0)
                continue;
            auto const where = cc::format("pixel ({}, {})", x, y);
            auto const v = (c[0] + 1.0f) / 2.0f;
            auto const w = (c[1] + 1.0f) / 2.0f;
            auto const got = capped[0].rgba_float(x, y);
            CHECK((near(got[0], 1.0f - v - w, 0.001f) && near(got[1], v, 0.001f) && near(got[2], w, 0.001f)))
                .context(cc::format("{}: ({}, {}, {})", where, got[0], got[1], got[2]));
            // the evaluation stage's factors are the capped ones on every target
            CHECK(got[3] == 1.0f).context(where);
            auto const fine = finer[0].rgba_float(x, y);
            CHECK(fine[3] == 4.0f).context(where);
            if (!near(fine[1], v, 0.05f))
                ++bent;
        }
    CHECK(bent > 0);
}

ASYNC_INVOCABLE_TEST("sg - a tessellated quad patch covers its quad, reports its domain, and winds as its control "
                     "stage says",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_tessellation(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = tessellation_polyfilling(*ctx);

    // The patch is the middle half of the target, its corners counter-clockwise from the bottom left.
    // The evaluation stage blends them by `uv`, along the bottom edge and then upwards.
    shaders::stages::stage_corner const corners[] = {{.position = tg::vec2f(-0.5f, -0.5f)},
                                                     {.position = tg::vec2f(0.5f, -0.5f)},
                                                     {.position = tg::vec2f(0.5f, 0.5f)},
                                                     {.position = tg::vec2f(-0.5f, 0.5f)}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const draw_with = [&](auto const& declared) -> cc::shared_async<sg_test::offscreen_pixels>
    {
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(declared);
        co_return co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.draw({.vertex_range = {.offset = 0, .size = 4}});
                                                   });
    };
    auto const ccw = co_await draw_with(shaders::stages::quad_ccw);
    auto const cw = co_await draw_with(shaders::stages::quad_cw);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const c = centre_of(x, y);
            // pixel centres sit a quarter pixel off the patch's edges, so each is clearly in or out
            auto const inside = c[0] > -0.5f && c[0] < 0.5f && c[1] > -0.5f && c[1] < 0.5f;
            auto const where = cc::format("pixel ({}, {})", x, y);
            CHECK(drawn(ccw, x, y) == inside).context(where);
            CHECK(!drawn(cw, x, y)).context(where);
            if (inside)
            {
                auto const got = ccw[0].rgba_float(x, y);
                auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
                CHECK((near(got[0], c[0] + 0.5f) && near(got[1], c[1] + 0.5f)))
                    .context(cc::format("{}: domain ({}, {})", where, got[0], got[1]));
            }
        }
}

ASYNC_INVOCABLE_TEST("sg - tessellated patches of differing factors each draw their own points, past one workgroup "
                     "of the scan",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_tessellation(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = tessellation_polyfilling(*ctx);

    // 32 x 32 patches of 2 x 2 pixels over a 64 x 64 target, 1024 of them, so the polyfill's scan walks four chunks.
    // Factors 1 to 5 by place give each patch its own point and index counts, so an offset off by one patch shows as
    // a pixel left out, or one colored by another patch's `uv` or left edge.
    constexpr auto grid = 32;
    constexpr auto pixels_wide = 2 * grid;
    auto corners = cc::vector<shaders::stages::stage_corner>();
    auto const step = 2.0f / float(grid);
    for (auto row = 0; row < grid; ++row)
        for (auto column = 0; column < grid; ++column)
        {
            auto const at = tg::vec2f(-1.0f + float(column) * step, -1.0f + float(row) * step);
            corners.push_back({.position = at});
            corners.push_back({.position = at + tg::vec2f(step, 0)});
            corners.push_back({.position = at + tg::vec2f(step, step)});
            corners.push_back({.position = at + tg::vec2f(0, step)});
        }
    auto const count = corners.size();
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(corners), sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::quad_grid);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = pixels_wide,
                                                          .height = pixels_wide,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::stages::stage_target::name,
                                                          .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = count}});
                                                         });

    auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
    for (auto y = 0; y < pixels_wide; ++y)
        for (auto x = 0; x < pixels_wide; ++x)
        {
            auto const got = pixels[0].rgba_float(x, y);
            // pixel rows count down from the top, and a patch's v up from its bottom
            auto const u = (x % 2 == 0) ? 0.25f : 0.75f;
            auto const v = (y % 2 == 0) ? 0.75f : 0.25f;
            auto const left = -1.0f + float(x / 2) * step;
            CHECK((got[3] == 1.0f && near(got[0], u) && near(got[1], v) && near(got[2], left)))
                .context(cc::format("pixel ({}, {}): ({}, {}, {}, {})", x, y, got[0], got[1], got[2], got[3]));
        }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage's ended strips draw each copy and nothing between them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // One triangle in the left half, which the geometry stage emits again one unit to the right.
        // Without the strip ended between them, the strip's middle triangle would bridge the two across the bottom.
        auto const a = tg::vec2f(-1, -1);
        auto const b = tg::vec2f(0, -1);
        auto const c = tg::vec2f(-1, 1);
        auto const shift = tg::vec2f(1, 0);
        shaders::stages::stage_corner const corners[] = {{.position = a}, {.position = b}, {.position = c}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::doubled);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                             });

        auto bridged = 0;
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const p = centre_of(x, y);
                auto const first = side_of(p, a, b, c);
                auto const second = side_of(p, a + shift, b + shift, c + shift);
                auto const bridge = side_of(p, c, a + shift, b + shift);
                if (first == 0 || second == 0 || bridge == 0)
                    continue;
                auto const inside = first > 0 || second > 0;
                bridged += !inside && bridge > 0 ? 1 : 0;
                CHECK(drawn(pixels, x, y) == inside).context(cc::format("{}: pixel ({}, {})", by, x, y));
            }
        CHECK(bridged > 0); // the probes include pixels only a bridging triangle would reach
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage that appends lines draws each triangle's outline and nothing inside it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // One triangle well inside the target, which the geometry stage draws as its three edges and no more.
        // Each primitive leaves two of its five line slots unwritten, which the mesh route fills with lines that draw nothing.
        auto const a = tg::vec2f(-0.75f, -0.75f);
        auto const b = tg::vec2f(0.75f, -0.75f);
        auto const c = tg::vec2f(0.0f, 0.75f);
        shaders::stages::stage_corner const corners[] = {{.position = a}, {.position = b}, {.position = c}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::outlined);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                             });

        // in pixels, how far a pixel's centre lies from the nearest edge
        auto const pixel = 2.0f / float(size);
        auto const distance_to_edges = [&](tg::vec2f p)
        {
            auto best = 1e9f;
            tg::vec2f const ends[][2] = {{a, b}, {b, c}, {c, a}};
            for (auto const& e : ends)
            {
                auto const d = e[1] - e[0];
                auto const t
                    = cc::clamp(((p - e[0])[0] * d[0] + (p - e[0])[1] * d[1]) / (d[0] * d[0] + d[1] * d[1]), 0.0f, 1.0f);
                auto const q = e[0] + d * t;
                best = cc::min(best, (p - q).length() / pixel);
            }
            return best;
        };
        auto lit = 0;
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                if (!drawn(pixels, x, y))
                    continue;
                ++lit;
                CHECK(distance_to_edges(centre_of(x, y)) < 1.5f)
                    .context(cc::format("{}: pixel ({}, {}) is lit away from every edge", by, x, y));
            }
        // each edge's middle is drawn: a pixel within one of it is lit
        tg::vec2f const middles[] = {(a + b) * 0.5f, (b + c) * 0.5f, (c + a) * 0.5f};
        for (auto const& mid : middles)
        {
            auto found = false;
            for (auto y = 0; y < size; ++y)
                for (auto x = 0; x < size; ++x)
                    if (drawn(pixels, x, y) && (centre_of(x, y) - mid).length() / pixel < 1.0f)
                        found = true;
            CHECK(found).context(cc::format("{}: no pixel lit at the edge middle ({}, {})", by, mid[0], mid[1]));
        }
        CHECK(lit > 3 * 8).context(by); // three edges of some 10 to 20 pixels each, not a few stray ones
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage that appends points draws each corner's pixel and no other",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route asked for" : "the compute route";

        // three corners on three pixels' centres, each drawn as a point of one pixel
        int const xs[] = {3, 12, 8};
        int const ys[] = {12, 12, 3};
        shaders::stages::stage_corner const corners[] = {{.position = centre_of(xs[0], ys[0])},
                                                         {.position = centre_of(xs[1], ys[1])},
                                                         {.position = centre_of(xs[2], ys[2])}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::cornered);
        // a mesh stage writes lines or triangles alone, so points take the compute route whichever was asked for
        CHECK(is_built_by(*ctx, *pipeline, sg::geometry_polyfill_route::compute)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                             });
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const corner
                    = (x == xs[0] && y == ys[0]) || (x == xs[1] && y == ys[1]) || (x == xs[2] && y == ys[2]);
                CHECK(drawn(pixels, x, y) == corner).context(cc::format("{}: pixel ({}, {})", by, x, y));
            }
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage over a triangle strip draws both triangles of a quad, each wound alike",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // a quad as a strip of two triangles; back faces are culled, so the second draws only if its first two corners
        // are swapped as a strip's odd triangle's are, winding it as the first
        auto const a = tg::vec2f(-0.75f, -0.75f);
        auto const b = tg::vec2f(0.75f, -0.75f);
        auto const c = tg::vec2f(-0.75f, 0.75f);
        auto const d = tg::vec2f(0.75f, 0.75f);
        shaders::stages::stage_corner const corners[]
            = {{.position = a}, {.position = b}, {.position = c}, {.position = d}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::stripped);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 4}});
                                                             });
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const p = centre_of(x, y);
                auto const first = side_of(p, a, b, c);
                auto const second = side_of(p, b, d, c);
                if (first == 0 || second == 0)
                    continue;
                CHECK(drawn(pixels, x, y) == (first > 0 || second > 0)).context(cc::format("{}: pixel ({}, {})", by, x, y));
            }
    }
}

ASYNC_INVOCABLE_TEST("sg - an indexed triangle strip through a geometry stage begins a new strip at its restart index",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // a quad on the left as a strip of four indices, the restart index, and a triangle on the right: no primitive
        // spans the restart, so nothing lies between the two or outside them
        // Back faces are culled, and the right triangle begins a strip at position 5: as the first of its strip it is
        // even, and unswapped it winds as the quad's, where counting from the draw's start would swap it away.
        auto const a = tg::vec2f(-0.875f, -0.75f);
        auto const b = tg::vec2f(-0.125f, -0.75f);
        auto const c = tg::vec2f(-0.875f, 0.75f);
        auto const d = tg::vec2f(-0.125f, 0.75f);
        auto const e = tg::vec2f(0.125f, -0.75f);
        auto const f = tg::vec2f(0.875f, -0.75f);
        auto const g = tg::vec2f(0.5f, 0.75f);
        shaders::stages::stage_corner const corners[]
            = {{.position = a}, {.position = b}, {.position = c}, {.position = d},
               {.position = e}, {.position = f}, {.position = g}};
        u16 const short_indices[] = {0, 1, 2, 3, 0xffff, 4, 5, 6};
        u32 const long_indices[] = {0, 1, 2, 3, 0xffffffffu, 4, 5, 6};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const shorts = ctx->persistent.create_buffer_from_data(short_indices, sg::buffer_usage::index_buffer);
        auto const longs = ctx->persistent.create_buffer_from_data(long_indices, sg::buffer_usage::index_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::stripped);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        for (auto const is_short : {true, false})
        {
            auto const kind = cc::format("{}, {}-bit indices", by, is_short ? 16 : 32);
            auto const pixels = co_await sg_test::draw_offscreen(
                *ctx, stage_target(),
                [&](sg::rendering_scope& scope)
                {
                    scope.bind_pipeline(*pipeline);
                    scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                    scope.bind_index_buffer(is_short ? shorts.as_index_buffer() : longs.as_index_buffer());
                    scope.draw_indexed({.index_range = {.offset = 0, .size = 8}});
                });
            for (auto y = 0; y < size; ++y)
                for (auto x = 0; x < size; ++x)
                {
                    auto const p = centre_of(x, y);
                    auto const left = side_of(p, a, b, c);
                    auto const left_second = side_of(p, b, d, c);
                    auto const right = side_of(p, e, f, g);
                    if (left == 0 || left_second == 0 || right == 0)
                        continue;
                    CHECK(drawn(pixels, x, y) == (left > 0 || left_second > 0 || right > 0))
                        .context(cc::format("{}: pixel ({}, {})", kind, x, y));
                }
        }
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage's primitive id counts the primitives an indexed strip makes past its "
                     "restart",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // the restart test's strips: the quad's two triangles are primitives 0 and 1, and the right triangle, the
        // fifth window, is primitive 2, since the three windows over the restart make none
        auto const a = tg::vec2f(-0.875f, -0.75f);
        auto const b = tg::vec2f(-0.125f, -0.75f);
        auto const c = tg::vec2f(-0.875f, 0.75f);
        auto const d = tg::vec2f(-0.125f, 0.75f);
        auto const e = tg::vec2f(0.125f, -0.75f);
        auto const f = tg::vec2f(0.875f, -0.75f);
        auto const g = tg::vec2f(0.5f, 0.75f);
        shaders::stages::stage_corner const corners[]
            = {{.position = a}, {.position = b}, {.position = c}, {.position = d},
               {.position = e}, {.position = f}, {.position = g}};
        u16 const indices[] = {0, 1, 2, 3, 0xffff, 4, 5, 6};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const index_buffer = ctx->persistent.create_buffer_from_data(indices, sg::buffer_usage::index_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::strip_primitive_ids);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels
            = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                               [&](sg::rendering_scope& scope)
                                               {
                                                   scope.bind_pipeline(*pipeline);
                                                   scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                   scope.bind_index_buffer(index_buffer.as_index_buffer());
                                                   scope.draw_indexed({.index_range = {.offset = 0, .size = 8}});
                                               });
        auto checked = 0;
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const p = centre_of(x, y);
                auto const in = [&](tg::vec2f u, tg::vec2f v, tg::vec2f w) { return side_of(p, u, v, w); };
                auto const expect = in(a, b, c) > 0 ? 0 : in(b, d, c) > 0 ? 1 : in(e, f, g) > 0 ? 2 : -1;
                if (expect < 0 || in(a, b, c) == 0 || in(b, d, c) == 0 || in(e, f, g) == 0)
                    continue;
                ++checked;
                CHECK(pixels[0].rgba_float(x, y)[0] == float(expect)).context(cc::format("{}: pixel ({}, {})", by, x, y));
            }
        CHECK(checked > 20).context(by);
    }
}

ASYNC_INVOCABLE_TEST("sg - an indexed strip's primitive ids and windings hold past one workgroup of the strip scan",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);

    // A 64 x 64 target, each pixel row cut into strips of irregular length, a restart between them: over 8000 windows,
    // so the polyfill's strip scan walks over thirty chunks of 256 and carries its counts and its last restart across.
    // A strip zigzags top, bottom, top, ... across its cells, the tops a quarter pixel right, so a pixel's centre lies
    // in its cell's first triangle, window 2 x within the strip, wound counter-clockwise as the even windows are.
    // With back faces culled, a window given the wrong parity is swapped, wound the other way, and draws nothing.
    constexpr auto wide = 64;
    auto corners = cc::vector<shaders::stages::stage_corner>();
    auto indices = cc::vector<u32>();
    auto expected = cc::vector<int>::create_filled(wide * wide, -1);
    auto const clip = [&](float x, float y) { return tg::vec2f(-1.0f + 2.0f * x / wide, 1.0f - 2.0f * y / wide); };
    auto made = 0;
    for (auto row = 0; row < wide; ++row)
    {
        auto const top_of = [&](int x)
        {
            corners.push_back({.position = clip(float(x) + 0.25f, float(row))});
            return u32(corners.size() - 1);
        };
        auto const bottom_of = [&](int x)
        {
            corners.push_back({.position = clip(float(x), float(row + 1))});
            return u32(corners.size() - 1);
        };
        auto x0 = 0;
        while (x0 < wide)
        {
            auto const x1 = cc::min(wide, x0 + 1 + (x0 * 7 + row * 13) % 23);
            for (auto x = x0; x <= x1; ++x)
            {
                indices.push_back(top_of(x));
                indices.push_back(bottom_of(x));
            }
            for (auto x = x0; x < x1; ++x)
            {
                expected[row * wide + x] = made;
                made += 2;
            }
            indices.push_back(0xffffffffu);
            x0 = x1;
        }
    }
    CHECK(made > 8000);
    auto const count = indices.size();
    auto const buffer = ctx->persistent.create_buffer_from_data(cc::move(corners), sg::buffer_usage::vertex_buffer);
    auto const index_buffer = ctx->persistent.create_buffer_from_data(cc::move(indices), sg::buffer_usage::index_buffer);

    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";
        for (auto const is_culled : {false, true})
        {
            auto const pipeline
                = is_culled ? co_await ctx->cached.acquire_raster_pipeline(shaders::stages::strip_primitive_ids_culled)
                            : co_await ctx->cached.acquire_raster_pipeline(shaders::stages::strip_primitive_ids);
            CHECK(is_built_by(*ctx, *pipeline, route)).context(by);
            auto const kind = cc::format("{}, {}", by, is_culled ? "culled" : "not culled");
            auto const pixels
                = co_await sg_test::draw_offscreen(*ctx,
                                                   {.width = wide,
                                                    .height = wide,
                                                    .colors = {sg::pixel_format::rgba32_float},
                                                    .target_set = shaders::stages::stage_target::name,
                                                    .clear_color = tg::vec4f(-1, -1, -1, -1)},
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.bind_index_buffer(index_buffer.as_index_buffer());
                                                       scope.draw_indexed({.index_range = {.offset = 0, .size = count}});
                                                   });
            for (auto y = 0; y < wide; ++y)
                for (auto x = 0; x < wide; ++x)
                {
                    auto const got = pixels[0].rgba_float(x, y)[0];
                    CHECK(got == float(expected[y * wide + x]))
                        .context(cc::format("{}: pixel ({}, {}) is {}", kind, x, y, got));
                }
        }
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage over a line strip draws each segment of the loop",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // a closed loop of four vertices, three segments: a triangle's outline, and nothing inside it
        auto const a = tg::vec2f(-0.75f, -0.75f);
        auto const b = tg::vec2f(0.75f, -0.75f);
        auto const c = tg::vec2f(0.0f, 0.75f);
        shaders::stages::stage_corner const corners[]
            = {{.position = a}, {.position = b}, {.position = c}, {.position = a}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::line_stripped);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 4}});
                                                             });
        auto const pixel = 2.0f / float(size);
        auto const distance_to_edges = [&](tg::vec2f p)
        {
            auto best = 1e9f;
            tg::vec2f const ends[][2] = {{a, b}, {b, c}, {c, a}};
            for (auto const& e : ends)
            {
                auto const dir = e[1] - e[0];
                auto const t = cc::clamp(
                    ((p - e[0])[0] * dir[0] + (p - e[0])[1] * dir[1]) / (dir[0] * dir[0] + dir[1] * dir[1]), 0.0f, 1.0f);
                best = cc::min(best, (p - (e[0] + dir * t)).length() / pixel);
            }
            return best;
        };
        auto lit = 0;
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
                if (drawn(pixels, x, y))
                {
                    ++lit;
                    CHECK(distance_to_edges(centre_of(x, y)) < 1.5f)
                        .context(cc::format("{}: pixel ({}, {}) is lit away from every edge", by, x, y));
                }
        CHECK(lit > 3 * 8).context(by);
    }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage reads each triangle's primitive id and passes it on flat",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // Two triangles over the whole target: the lower-right is primitive 0 and the upper-left primitive 1.
        auto const bl = tg::vec2f(-1, -1);
        auto const br = tg::vec2f(1, -1);
        auto const tr = tg::vec2f(1, 1);
        auto const tl = tg::vec2f(-1, 1);
        shaders::stages::stage_corner const corners[] = {{.position = bl}, {.position = br}, {.position = tr},
                                                         {.position = bl}, {.position = tr}, {.position = tl}};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::primitive_ids);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                             });

        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const p = centre_of(x, y);
                auto const lower = side_of(p, bl, br, tr);
                auto const upper = side_of(p, bl, tr, tl);
                if (lower == 0 || upper == 0)
                    continue;
                CHECK(pixels[0].rgba_float(x, y)[0] == (lower > 0 ? 0.0f : 1.0f))
                    .context(cc::format("{}: pixel ({}, {})", by, x, y));
            }
    }
}

ASYNC_INVOCABLE_TEST("sg - an indexed draw through a geometry stage assembles each triangle from its indices",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = no_geometry_stage(*ctx); why.has_value())
        SKIP(why.value());
    auto const polyfill = geometry_polyfilling(*ctx);
    for (auto const route : geometry_routes_of(*ctx))
    {
        auto const routed = geometry_routing(*ctx, route);
        auto const by = route == sg::geometry_polyfill_route::mesh ? "the mesh route" : "the compute route";

        // The two triangles of the primitive-id test from four corners: 16-bit indices first, then 32-bit ones offset by a
        // decoy corner, each past an unused leading pair.
        auto const bl = tg::vec2f(-1, -1);
        auto const br = tg::vec2f(1, -1);
        auto const tr = tg::vec2f(1, 1);
        auto const tl = tg::vec2f(-1, 1);
        shaders::stages::stage_corner const corners[]
            = {{.position = tg::vec2f(5, 5)}, {.position = bl}, {.position = br}, {.position = tr}, {.position = tl}};
        u16 const short_indices[] = {0, 0, 1, 2, 3, 1, 3, 4};
        u32 const long_indices[] = {9, 9, 0, 1, 2, 0, 2, 3};
        auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
        auto const shorts = ctx->persistent.create_buffer_from_data(short_indices, sg::buffer_usage::index_buffer);
        auto const longs = ctx->persistent.create_buffer_from_data(long_indices, sg::buffer_usage::index_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::primitive_ids);
        CHECK(is_built_by(*ctx, *pipeline, route)).context(by);

        for (auto const is_short : {true, false})
        {
            auto const pixels = co_await sg_test::draw_offscreen(
                *ctx, stage_target(),
                [&](sg::rendering_scope& scope)
                {
                    scope.bind_pipeline(*pipeline);
                    scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                    scope.bind_index_buffer(is_short ? shorts.as_index_buffer() : longs.as_index_buffer());
                    scope.draw_indexed({.index_range = {.offset = 2, .size = 6}, .vertex_offset = is_short ? 0 : 1});
                });
            for (auto y = 0; y < size; ++y)
                for (auto x = 0; x < size; ++x)
                {
                    auto const p = centre_of(x, y);
                    auto const lower = side_of(p, bl, br, tr);
                    auto const upper = side_of(p, bl, tr, tl);
                    if (lower == 0 || upper == 0)
                        continue;
                    CHECK(pixels[0].rgba_float(x, y)[0] == (lower > 0 ? 0.0f : 1.0f))
                        .context(cc::format("{}, {} indices, pixel ({}, {})", by, is_short ? "16-bit" : "32-bit", x, y));
                }
        }
    }
}

ASYNC_INVOCABLE_TEST("sg - a pipeline with a geometry stage the device lacks is refused naming geometry_shader",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::geometry_shader))
        SKIP("this device has a geometry stage, so nothing is refused");

    auto const built = ctx->cached.acquire_raster_pipeline(shaders::stages::doubled);
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("geometry_shader"));
}

ASYNC_INVOCABLE_TEST("sg - a pipeline with tessellation stages the device lacks is refused naming tessellation_shader",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::tessellation_shader))
        SKIP("this device has tessellation stages, so nothing is refused");
    if (ctx->tessellation_polyfill() != sg::polyfill_use::never)
        SKIP("this context polyfills tessellation, so nothing is refused");

    auto const built = ctx->cached.acquire_raster_pipeline(shaders::stages::tessellated_ccw);
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("tessellation_shader"));
}

ASYNC_INVOCABLE_TEST("sg - a wireframe pipeline on a device without wireframe fill is refused naming wireframe_fill",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::wireframe_fill))
        SKIP("this device has wireframe fill, so nothing is refused");

    auto const built
        = ctx->cached.acquire_raster_pipeline(shaders::rects::floating, {}, [](sg::raster_pipeline_description& d)
                                              { d.rasterization.fill = sg::fill_mode::wireframe; });
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("wireframe_fill"));
}

namespace
{
/// Whether pixel (x, y) of a clipped draw holds `color` right of the centre and the clear color left of it.
void check_right_half(sg_test::offscreen_pixels const& pixels, tg::vec4f color)
{
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba_float(x, y) == (x < size / 2 ? tg::vec4f(-1, -1, -1, -1) : color))
                .context(cc::format("pixel ({}, {})", x, y));
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL evaluation stage's clip distances cut the tessellated quad the rasterizer draws",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::tessellation_shader))
        SKIP("this device has no tessellation stages, and no polyfill carries clip distances");
    if (!ctx->supports(sg::feature::clip_distances))
        SKIP("this context clips by no distance a stage writes");

    // the patch covers the target, and its second clip distance is x: the left half is cut away
    shaders::stages::stage_corner const corners[] = {{.position = tg::vec2f(-1, -1)},
                                                     {.position = tg::vec2f(1, -1)},
                                                     {.position = tg::vec2f(1, 1)},
                                                     {.position = tg::vec2f(-1, 1)}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::clipped_quad);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 4}});
                                                         });
    // the color after the lanes reaches the pixel stage as written, which a link off by a register would not
    check_right_half(pixels, tg::vec4f(0, 1, 0, 1));
}

ASYNC_INVOCABLE_TEST("sg - an SGL geometry stage's clip distances cut each triangle it appends",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::geometry_shader))
        SKIP("this device has no geometry stage, and no polyfill carries clip distances");
    if (!ctx->supports(sg::feature::clip_distances))
        SKIP("this context clips by no distance a stage writes");

    // two triangles over the target, whose second clip distance is x: the left half is cut away
    shaders::stages::stage_corner const corners[]
        = {{.position = tg::vec2f(-1, -1)}, {.position = tg::vec2f(1, -1)}, {.position = tg::vec2f(1, 1)},
           {.position = tg::vec2f(-1, -1)}, {.position = tg::vec2f(1, 1)},  {.position = tg::vec2f(-1, 1)}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages::clipped_triangles);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });
    check_right_half(pixels, tg::vec4f(0, 0, 1, 1));
}

ASYNC_INVOCABLE_TEST("sg - a polyfilled geometry stage that writes clip distances is refused, naming them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->draws_geometry_polyfill())
        SKIP("this backend draws no polyfilled geometry stage");

    // the polyfill is what runs whether or not the device has a geometry stage
    auto const before = ctx->geometry_polyfill();
    ctx->set_geometry_polyfill(sg::polyfill_use::always);
    auto const built = ctx->cached.acquire_raster_pipeline(shaders::stages::clipped_triangles);
    co_await cc::async_settled(built);
    ctx->set_geometry_polyfill(before);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("clip distances"))
        .dump("error", built->try_error()->underlying().to_string());
}
