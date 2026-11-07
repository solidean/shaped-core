#include "../pipeline/pipeline_harness.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// A vertex stage of one route against a pixel stage of the other, on a real pipeline.
// The packer's golden test says each side matches DXC's table; only a draw says the two sides link and agree in value.

namespace
{
constexpr int k_size = 8;
constexpr float k_tolerance = 1e-4f;

template <class Asset>
cc::shared_async<cc::optional<sg::compiled_shader>> compiled(sg::context& ctx, Asset const& asset)
{
    auto const acquired = asset->acquire(ctx);
    co_await cc::async_settled(acquired);
    auto const* const value = acquired->try_value();
    if (value == nullptr)
        co_return cc::optional<sg::compiled_shader>();
    co_return *value;
}

/// The stage SGL's own binary gives, in whichever format this context takes.
template <class Dxil, class Spirv>
cc::shared_async<cc::optional<sg::compiled_shader>> compiled_native(sg::context& ctx,
                                                                    bool takes_dxil,
                                                                    Dxil const& dxil_asset,
                                                                    Spirv const& spirv_asset)
{
    if (takes_dxil)
        co_return co_await compiled(ctx, dxil_asset);
    co_return co_await compiled(ctx, spirv_asset);
}

sg::raster_pipeline_handle link_pipeline(sg::context& ctx,
                                         sg::compiled_shader const& vertex,
                                         sg::compiled_shader const& pixel)
{
    auto desc = sg::raster_pipeline_description{
        .layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{}),
        .vertex_shader = vertex,
        .fragment_shader = pixel,
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba32_float});
    desc.color_targets.push_back({.format = sg::pixel_format::rgba32_float});
    return ctx.uncached.create_raster_pipeline(desc);
}

cc::shared_async<sg_test::offscreen_pixels> drawn(sg::context& ctx,
                                                  sg::compiled_shader const& vertex,
                                                  sg::compiled_shader const& pixel)
{
    auto const pipeline = link_pipeline(ctx, vertex, pixel);
    co_return co_await sg_test::draw_offscreen(
        ctx,
        {.width = k_size, .height = k_size, .colors = {sg::pixel_format::rgba32_float, sg::pixel_format::rgba32_float}},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.draw({.vertex_range = {.offset = 0, .size = 3}, .instance_range = {.offset = 2, .size = 1}});
        });
}

/// One pixel stage's four stages, its two routes each.
struct routes
{
    cc::optional<sg::compiled_shader> vertex_text;
    cc::optional<sg::compiled_shader> pixel_text;
    cc::optional<sg::compiled_shader> vertex_native;
    cc::optional<sg::compiled_shader> pixel_native;
};

/// What a draw of the surface's pixel stages differs by.
struct surface_draw
{
    sg::pipeline_layout_handle layout;
    sg::binding_group const* group = nullptr;
    /// Tests the depth a stage writes against a cleared target of it.
    bool has_depth = false;
};

cc::shared_async<sg_test::offscreen_pixels> drawn_surface(sg::context& ctx,
                                                          sg::compiled_shader const& vertex,
                                                          sg::compiled_shader const& pixel,
                                                          surface_draw const& how)
{
    auto desc = sg::raster_pipeline_description{
        .layout = how.layout,
        .vertex_shader = vertex,
        .fragment_shader = pixel,
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba32_float});
    desc.color_targets.push_back({.format = sg::pixel_format::rgba32_float});
    if (how.has_depth)
    {
        desc.depth_stencil_format = sg::pixel_format::depth32_float;
        desc.depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::less};
    }
    auto const pipeline = ctx.uncached.create_raster_pipeline(desc);
    auto const colors = sg_test::offscreen{
        .width = k_size,
        .height = k_size,
        .colors = {sg::pixel_format::rgba32_float, sg::pixel_format::rgba32_float},
        .depth_stencil = how.has_depth ? cc::optional<sg::pixel_format>(sg::pixel_format::depth32_float)
                                       : cc::optional<sg::pixel_format>(),
        .clear_depth = 0.5f};
    co_return co_await sg_test::draw_offscreen(ctx, colors,
                                               [&](sg::rendering_scope& scope)
                                               {
                                                   scope.bind_pipeline(*pipeline);
                                                   if (how.group != nullptr)
                                                       scope.bind_group(0, *how.group);
                                                   scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                               });
}

void check_same(sg_test::offscreen_pixels const& seen, sg_test::offscreen_pixels const& reference, cc::string_view route)
{
    for (auto target = 0; target < 2; ++target)
        for (auto y = 0; y < k_size; ++y)
            for (auto x = 0; x < k_size; ++x)
            {
                auto const a = seen[target].rgba_float(x, y);
                auto const b = reference[target].rgba_float(x, y);
                for (auto lane = 0; lane < 4; ++lane)
                {
                    auto const gap = a[lane] - b[lane];
                    CHECK((gap <= k_tolerance && gap >= -k_tolerance))
                        .context(cc::format("{}: target {} pixel ({}, {}) lane {}", route, target, x, y, lane));
                }
            }
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a stage of SGL's own binary links against one through DXC, both ways round, and draws alike",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    (void)sg_test::shader_fixtures();
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to mix with");

    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    auto const takes_spirv = ctx->accepts_shader_format(sg::shader_format::spirv);
    if (!takes_dxil && !takes_spirv)
        SKIP("this context takes neither DXIL nor SPIR-V");

    auto const vertex_text = co_await compiled(*ctx, sg::test::mixed_text::mixed_link::main_vs);
    auto const pixel_text = co_await compiled(*ctx, sg::test::mixed_text::mixed_link::main_ps);
    auto const vertex_native = co_await compiled_native(*ctx, takes_dxil, sg::test::mixed_dxil::mixed_link::main_vs,
                                                        sg::test::mixed_spirv::mixed_link::main_vs);
    auto const pixel_native = co_await compiled_native(*ctx, takes_dxil, sg::test::mixed_dxil::mixed_link::main_ps,
                                                       sg::test::mixed_spirv::mixed_link::main_ps);
    if (!vertex_text.has_value() || !pixel_text.has_value())
        SKIP("this build has no DXC");
    REQUIRE(vertex_native.has_value());
    REQUIRE(pixel_native.has_value());
    // a pass below means nothing unless the two routes really are two
    CHECK(vertex_text.value().compiler.name != "sgl");
    CHECK(pixel_text.value().compiler.name != "sgl");
    CHECK(vertex_native.value().compiler.name == "sgl");
    CHECK(pixel_native.value().compiler.name == "sgl");

    auto const reference = co_await drawn(*ctx, vertex_text.value(), pixel_text.value());
    // id = 7 + the instance: the draw reached the pixel stage, so a wrong link cannot pass as two blank images
    CHECK(reference[1].rgba_float(k_size / 2, k_size / 2)[2] == 9.0f);

    check_same(co_await drawn(*ctx, vertex_native.value(), pixel_text.value()), reference, "native vertex, DXC pixel");
    check_same(co_await drawn(*ctx, vertex_text.value(), pixel_native.value()), reference, "DXC vertex, native pixel");
    check_same(co_await drawn(*ctx, vertex_native.value(), pixel_native.value()), reference, "both native");
}

namespace
{
/// Draws one pixel stage through every pairing of the routes, and holds each to the pair through DXC.
cc::shared_async<sg_test::offscreen_pixels> compared_routes(sg::context& ctx,
                                                            routes const& r,
                                                            surface_draw const& how,
                                                            cc::string_view name)
{
    REQUIRE(r.vertex_text.has_value());
    REQUIRE(r.pixel_text.has_value());
    REQUIRE(r.vertex_native.has_value());
    REQUIRE(r.pixel_native.has_value());
    CHECK(r.pixel_text.value().compiler.name != "sgl");
    CHECK(r.pixel_native.value().compiler.name == "sgl");
    auto const reference = co_await drawn_surface(ctx, r.vertex_text.value(), r.pixel_text.value(), how);
    check_same(co_await drawn_surface(ctx, r.vertex_native.value(), r.pixel_text.value(), how), reference,
               cc::format("{}: native vertex, DXC pixel", name));
    check_same(co_await drawn_surface(ctx, r.vertex_text.value(), r.pixel_native.value(), how), reference,
               cc::format("{}: DXC vertex, native pixel", name));
    check_same(co_await drawn_surface(ctx, r.vertex_native.value(), r.pixel_native.value(), how), reference,
               cc::format("{}: both native", name));
    co_return reference;
}

/// `compared_routes` for the pixel stage these assets hold, over the vertex stages of `vertex`.
template <class Text, class Dxil, class Spirv>
cc::shared_async<sg_test::offscreen_pixels> compared_stage(sg::context& ctx,
                                                           bool takes_dxil,
                                                           routes vertex,
                                                           Text const& text_asset,
                                                           Dxil const& dxil_asset,
                                                           Spirv const& spirv_asset,
                                                           surface_draw how,
                                                           cc::string_view name)
{
    vertex.pixel_text = co_await compiled(ctx, text_asset);
    vertex.pixel_native = co_await compiled_native(ctx, takes_dxil, dxil_asset, spirv_asset);
    co_return co_await compared_routes(ctx, vertex, how, name);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - what a pixel stage reaches reads alike through SGL's binary and through DXC",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    (void)sg_test::shader_fixtures();
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to mix with");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (!takes_dxil && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("this context takes neither DXIL nor SPIR-V");
    namespace text = sg::test::mixed_text::pixel_surface;
    namespace dxil = sg::test::mixed_dxil::pixel_surface;
    namespace spirv = sg::test::mixed_spirv::pixel_surface;
    auto const probe = co_await compiled(*ctx, text::surface_vs);
    if (!probe.has_value())
        SKIP("this build has no DXC");
    // Workaround: a co_await inside this initializer crashes MSVC 14.44 and 14.51 (C1001).
    auto const native_vertex = co_await compiled_native(*ctx, takes_dxil, dxil::surface_vs, spirv::surface_vs);
    auto const vertex = routes{.vertex_text = probe, .vertex_native = native_vertex};

    auto const empty = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{});
    auto const discarding = co_await compared_stage(*ctx, takes_dxil, vertex, text::discarding_ps, dxil::discarding_ps,
                                                    spirv::discarding_ps, {.layout = empty}, "discard");
    // the discarded column stays as it was cleared, and its neighbour's derivative still read it, as a helper
    CHECK(discarding[0].rgba_float(0, k_size / 2)[3] == 0.0f);
    CHECK(discarding[0].rgba_float(1, k_size / 2)[0] == 2.0f);
    CHECK(discarding[0].rgba_float(3, k_size / 2)[0] == 6.0f);

    auto const inputs = co_await compared_stage(*ctx, takes_dxil, vertex, text::inputs_ps, dxil::inputs_ps,
                                                spirv::inputs_ps, {.layout = empty}, "inputs");
    CHECK(inputs[1].rgba_float(k_size / 2, k_size / 2)[3] == 1.0f); // the one sample is covered

    auto const depth = co_await compared_stage(*ctx, takes_dxil, vertex, text::depth_ps, dxil::depth_ps,
                                               spirv::depth_ps, {.layout = empty, .has_depth = true}, "depth");
    // the written depth is an eighth of x against the 0.5 it clears to, and every interpolated one is 0.5
    CHECK(depth[1].rgba_float(1, k_size / 2)[0] == 1.0f);
    CHECK(depth[1].rgba_float(k_size - 1, k_size / 2)[0] == 0.0f);

    // Four texels of a ramp, and a depth cleared to 0.5, each compare's reference on one side of it.
    constexpr int extent = 4;
    auto texels = cc::vector<byte>();
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
            texels.push_back_range(cc::span<byte const>({byte(40 + 60 * x), byte(200 - 50 * y), byte(10 * x), byte(255)}));
    auto const albedo
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = extent,
                                             .height = extent,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto const shadow
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                             .width = extent,
                                             .height = extent,
                                             .usage = sg::texture_usage::depth_stencil | sg::texture_usage::texture});
    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(albedo.raw(), cc::span<byte const>(texels));
    {
        auto scope = cmd->raster.render_to({.depth_stencil_target = shadow.as_depth_stencil_view().cleared(0.5f)});
    }
    auto const group_layout = ctx->cached.acquire_binding_group_layout<text::surface_textures>();
    auto const group = ctx->persistent.create_binding_group(
        group_layout, text::surface_textures{.albedo = albedo.as_texture_view(),
                                             .shadow = shadow.as_texture_view(),
                                             .smooth = sg::sampler{.min_filter = sg::sampler_filter::linear,
                                                                   .mag_filter = sg::sampler_filter::linear,
                                                                   .mip_filter = sg::sampler_filter::nearest},
                                             .compared = sg::sampler{.min_filter = sg::sampler_filter::nearest,
                                                                     .mag_filter = sg::sampler_filter::nearest,
                                                                     .mip_filter = sg::sampler_filter::nearest,
                                                                     .compare = sg::compare_op::less}});
    ctx->submit_command_list(cc::move(cmd));
    auto const sampled
        = co_await compared_stage(*ctx, takes_dxil, vertex, text::sampled_ps, dxil::sampled_ps, spirv::sampled_ps,
                                  {.layout = text::sampled_ps.acquire_layout(*ctx), .group = &*group}, "sampling");
    auto const second = sampled[1].rgba_float(k_size / 2, k_size / 2);
    CHECK(second[3] == 1.0f); // the texels' alpha: a stage that sampled nothing reads 0
    CHECK(second[0] == 1.0f); // 0.25 < 0.5
    CHECK(second[1] == 0.0f); // 0.75 > 0.5
}
