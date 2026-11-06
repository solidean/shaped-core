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
    auto const vertex_native = takes_dxil ? co_await compiled(*ctx, sg::test::mixed_dxil::mixed_link::main_vs)
                                          : co_await compiled(*ctx, sg::test::mixed_spirv::mixed_link::main_vs);
    auto const pixel_native = takes_dxil ? co_await compiled(*ctx, sg::test::mixed_dxil::mixed_link::main_ps)
                                         : co_await compiled(*ctx, sg::test::mixed_spirv::mixed_link::main_ps);
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
