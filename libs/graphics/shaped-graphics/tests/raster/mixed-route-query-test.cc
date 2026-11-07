#include "../pipeline/pipeline_harness.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// An inline trace through SGL's own binary against the same trace through DXC, one scene, every read a query has.

namespace
{
constexpr int k_grid = 8;
constexpr int k_cell_words = 8;
constexpr float k_tolerance = 1e-4f;

namespace text = sg::test::mixed_text::query_stages;
namespace dxil = sg::test::mixed_dxil::query_stages;
namespace spirv = sg::test::mixed_spirv::query_stages;

struct scene
{
    sg::tlas_handle tlas;
    cc::vector<sg::blas_handle> held;
};

/// The square of two triangles, the two balls' boxes, and each again a turn and a scale away, one of them a back face.
template <class Buffer>
scene built_scene(sg::context& ctx, Buffer const& triangles, Buffer const& boxes)
{
    auto build = ctx.create_command_list();
    auto const square_input = sg::blas_triangles{.vertices = triangles.raw(), .vertex_count = 6, .is_opaque = false};
    auto const ball_input = sg::blas_aabbs{.aabbs = boxes.raw(), .aabb_count = 2, .is_opaque = false};
    auto const square = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&square_input, 1));
    auto const balls = build->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&ball_input, 1));
    sg::tlas_instance const instances[] = {
        {.blas = square, .instance_id = 40},
        {.blas = balls, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 0}, .instance_id = 50},
        // a quarter turn about y, so the square faces the other way and every row of the transform is its own
        {.blas = square, .transform = {0, 0, 2, 5, 0, 1, 0, 0, -2, 0, 0, 6}, .instance_id = 77},
    };
    auto result = scene{};
    result.tlas = build->raytracing.build_tlas(instances);
    result.held.push_back(square);
    result.held.push_back(balls);
    ctx.submit_command_list(cc::move(build));
    return result;
}

void check_same(cc::span<tg::vec4f const> got, cc::span<tg::vec4f const> reference, cc::string_view what)
{
    REQUIRE(got.size() == reference.size());
    for (auto i = isize(0); i < got.size(); ++i)
        for (auto lane = 0; lane < 4; ++lane)
        {
            auto const delta = tg::abs(got[i][lane] - reference[i][lane]);
            // two misses sum to the same infinity, and a NaN of both routes agrees
            auto const are_alike
                = got[i][lane] == reference[i][lane] || (cc::is_nan(got[i][lane]) && cc::is_nan(reference[i][lane]));
            CHECK((are_alike || delta < k_tolerance)).context(cc::format("{}: word {} lane {}", what, i, lane));
        }
}

template <class Asset>
cc::shared_async<cc::vector<tg::vec4f>> traced(sg::context_handle ctx, Asset const& asset, scene const& s)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    auto const pipeline = co_await asset.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<text::query_scene>();
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const cells = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(k_grid * k_grid * k_cell_words), usage);
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, text::query_scene{.world = s.tlas->as_view(), .cells = cells.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_grid, k_grid);
    auto const back = cmd->download.data_from_buffer(cells);
    ctx->submit_command_list(cc::move(cmd));
    auto const words = co_await back.data();
    co_return cc::vector<tg::vec4f>::create_copy_of(words);
}

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

cc::shared_async<sg_test::offscreen_pixels> drawn(sg::context_handle ctx,
                                                  sg::compiled_shader const& vertex,
                                                  sg::compiled_shader const& pixel,
                                                  scene const& s)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);
    auto const layout = ctx->cached.acquire_binding_group_layout<text::pixel_query_scene>();
    auto desc = sg::raster_pipeline_description{
        .layout = ctx->cached.acquire_pipeline_layout<text::pixel_query_scene>(),
        .vertex_shader = vertex,
        .fragment_shader = pixel,
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba32_float});
    auto const pipeline = ctx->uncached.create_raster_pipeline(desc);
    co_return co_await sg_test::draw_offscreen_passes(
        *ctx, {.width = k_grid, .height = k_grid, .colors = {sg::pixel_format::rgba32_float}},
        [&](sg::command_list& cmd, sg_test::offscreen_targets const& targets)
        {
            auto const group
                = ctx->transient.create_binding_group(cmd, layout, text::pixel_query_scene{.world = s.tlas->as_view()});
            auto scope = cmd.raster.render_to(targets.cleared());
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        });
}

void check_pixels(sg_test::offscreen_pixels const& got, sg_test::offscreen_pixels const& reference, cc::string_view what)
{
    for (auto y = 0; y < k_grid; ++y)
        for (auto x = 0; x < k_grid; ++x)
        {
            auto const a = got[0].rgba_float(x, y);
            auto const b = reference[0].rgba_float(x, y);
            for (auto lane = 0; lane < 4; ++lane)
                CHECK(tg::abs(a[lane] - b[lane]) < k_tolerance).context(cc::format("{}: pixel ({}, {})", what, x, y));
        }
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an inline trace reads alike through SGL's binary and through DXC, in compute and in a pixel "
                     "stage",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_query))
        SKIP("this device has no ray queries");
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to mix with");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (!takes_dxil && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("this context takes neither DXIL nor SPIR-V");

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    float const boxes[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 4, 2, 2};
    auto const triangle_input
        = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const box_input = ctx->persistent.create_buffer_from_data(boxes, sg::buffer_usage::accel_structure_build_input);
    auto const s = built_scene(*ctx, triangle_input, box_input);

    auto const reference = co_await traced(ctx, text::read_everything, s);
    auto const native
        = takes_dxil ? co_await traced(ctx, dxil::read_everything, s) : co_await traced(ctx, spirv::read_everything, s);
    check_same(native, reference, "compute");

    // the scene answers: a test that compares two blanks says nothing
    int kinds[3] = {};
    for (auto i = 0; i < k_grid * k_grid; ++i)
        ++kinds[int(reference[i * k_cell_words][0])];
    CHECK(kinds[0] > 0);
    CHECK(kinds[1] > 0);
    CHECK(kinds[2] > 0);

    auto const twice_reference = co_await traced(ctx, text::trace_twice, s);
    auto const twice
        = takes_dxil ? co_await traced(ctx, dxil::trace_twice, s) : co_await traced(ctx, spirv::trace_twice, s);
    check_same(twice, twice_reference, "a query in a loop");

    auto const vertex_text = co_await compiled(*ctx, text::query_vs);
    auto const pixel_text = co_await compiled(*ctx, text::query_ps);
    auto const vertex_native = co_await compiled_native(*ctx, takes_dxil, dxil::query_vs, spirv::query_vs);
    auto const pixel_native = co_await compiled_native(*ctx, takes_dxil, dxil::query_ps, spirv::query_ps);
    REQUIRE(vertex_text.has_value());
    REQUIRE(pixel_text.has_value());
    REQUIRE(vertex_native.has_value());
    REQUIRE(pixel_native.has_value());
    CHECK(pixel_text.value().compiler.name != "sgl");
    CHECK(pixel_native.value().compiler.name == "sgl");
    auto const drawn_reference = co_await drawn(ctx, vertex_text.value(), pixel_text.value(), s);
    check_pixels(co_await drawn(ctx, vertex_native.value(), pixel_text.value(), s), drawn_reference, "native vertex");
    check_pixels(co_await drawn(ctx, vertex_text.value(), pixel_native.value(), s), drawn_reference, "native pixel");
    check_pixels(co_await drawn(ctx, vertex_native.value(), pixel_native.value(), s), drawn_reference, "both native");
}
