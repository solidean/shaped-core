#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_sgl_shaders.hh>
#include <sg_test_shaders.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// tests/shaders/hlsl/mesh.hlsl's mesh and task stages, drawn where the device has them.
// Every probe is a pixel centre, and no edge of any triangle passes through one.

namespace
{
constexpr int size = 8;

/// The compiled form of `asset` for `ctx`, or none where no compiler reaches it.
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

/// A mesh-shaded pipeline of `mesh`, `task` where there is one, and `ps_main`, over the empty layout.
sg::raster_pipeline_description mesh_pipeline(sg::context& ctx,
                                              sg::compiled_shader const& mesh,
                                              sg::compiled_shader const* task,
                                              sg::compiled_shader const& pixel)
{
    auto desc = sg::raster_pipeline_description{
        .layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{}),
        .fragment_shader = pixel,
        .mesh_shader = mesh,
        .rasterization = {.cull = sg::cull_mode::none},
    };
    if (task != nullptr)
        desc.task_shader = *task;
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    return desc;
}

sg_test::offscreen mesh_target()
{
    return {.width = size, .height = size, .colors = {sg::pixel_format::rgba8_unorm}};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a mesh stage writes the vertices and the triangle the rasterizer draws",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");
    (void)sg_test::shader_fixtures();

    auto const mesh = co_await compiled(*ctx, sg::test::shaders::mesh.mesh.ms_main);
    auto const pixel = co_await compiled(*ctx, sg::test::shaders::mesh.fragment.ps_main);
    if (!mesh.has_value() || !pixel.has_value())
        SKIP("this context accepts no format the mesh shaders compile to");

    auto const pipeline = ctx->uncached.create_raster_pipeline(mesh_pipeline(*ctx, mesh.value(), nullptr, pixel.value()));
    CHECK(pipeline->is_mesh_shaded());
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(0, 255, 0, 255)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - a task stage launches mesh workgroups, and each one reads the payload it was handed",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");
    (void)sg_test::shader_fixtures();

    auto const task = co_await compiled(*ctx, sg::test::shaders::mesh.task.as_main);
    auto const mesh = co_await compiled(*ctx, sg::test::shaders::mesh.mesh.ms_halves);
    auto const pixel = co_await compiled(*ctx, sg::test::shaders::mesh.fragment.ps_main);
    if (!task.has_value() || !mesh.has_value() || !pixel.has_value())
        SKIP("this context accepts no format the mesh shaders compile to");

    auto const pipeline
        = ctx->uncached.create_raster_pipeline(mesh_pipeline(*ctx, mesh.value(), &task.value(), pixel.value()));
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    // workgroup 0 is the left half, red 0, and workgroup 1 the right one, red 1; both blue from the payload
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const red = x < size / 2 ? 0 : 255;
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(red, 0, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
        }
}
