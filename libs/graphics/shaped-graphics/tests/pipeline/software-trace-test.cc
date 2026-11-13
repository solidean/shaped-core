#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <sg_test_software_trace.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::software_trace;

// A package that opted in with SOFTWARE_TRACE, on every backend with ray queries.
// A square of two triangles rises by 2 over the frame's interval, and instance 1, 4 to its side, by 2 more, so a ray
// at time 0.5 sees the first at z = 1 and the second at z = 4.

namespace
{
constexpr int grid = 8;

/// The two poses, three floats per vertex, and what a build reads of them on the CPU.
float const start_pose[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
float const end_pose[] = {0, 0, 2, 4, 0, 2, 0, 4, 2, 4, 0, 2, 4, 4, 2, 0, 4, 2};

struct moving_scene
{
    sg::tlas_handle tlas;
    sg::blas_handle blas;
};

/// Records the scene's builds on `cmd`; `flags` says whether they write the software walk's structure.
[[nodiscard]] moving_scene build_scene(sg::context& ctx, sg::command_list& cmd, sg::accel_build_flags flags)
{
    auto const usage = sg::buffer_usage::accel_structure_build_input;
    auto const start = ctx.persistent.create_buffer_from_data(start_pose, usage);
    auto const end = ctx.persistent.create_buffer_from_data(end_pose, usage);

    auto triangles = sg::blas_triangles{.vertices = start.raw(), .vertex_count = 6};
    triangles.cpu_vertices = cc::as_bytes(cc::span<float const>(start_pose));
    triangles.later_poses.push_back(
        {.vertices = end.raw(), .cpu_vertices = cc::as_bytes(cc::span<float const>(end_pose))});
    auto const blas = cmd.raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1), flags);

    sg::tlas_instance instances[] = {
        {.blas = blas, .instance_id = 10},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20},
    };
    instances[1].transform_at_end = cc::fixed_array<float, 12>{1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 4};
    return {.tlas = cmd.raytracing.build_tlas(instances, flags), .blas = blas};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an opted-in package's inline trace meets what moves, at its ray's time",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_tracing_motion_blur))
        SKIP("this context has no ray-traced motion");
    // metal's pipeline traces take motion natively, and its inline traces walk in software as every other backend's
    if (ctx->backend() != sg::backend_kind::metal)
        CHECK(ctx->implementation_of(sg::feature::ray_tracing_motion_blur) == sg::feature_implementation::emulated);

    auto const pipeline = co_await shaders::software_trace::trace_midway.acquire_pipeline(*ctx);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const scene = build_scene(*ctx, *cmd, sg::accel_build_flag::software_trace);

    // A structure past the first pool's megabyte, so the scene above is traced in a pool that has grown since.
    auto const empty_vertices = cc::vector<float>::create_filled(9 * 30000, 0.0f);
    auto const empty_input = ctx->persistent.create_buffer_from_data(cc::span<float const>(empty_vertices),
                                                                     sg::buffer_usage::accel_structure_build_input);
    auto const empty = sg::blas_triangles{.vertices = empty_input.raw(),
                                          .vertex_count = 3 * 30000,
                                          .cpu_vertices = cc::as_bytes(cc::span<float const>(empty_vertices))};
    auto const large = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&empty, 1),
                                                  sg::accel_build_flag::software_trace);
    CHECK(large != nullptr);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::software_trace::scene>();
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout,
                                              shaders::software_trace::scene{.world = scene.tlas->as_view(),
                                                                             .hits = hits.as_readwrite_buffer(),
                                                                             .ids = ids.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(grid, grid);
    auto const hits_back = cmd->download.data_from_buffer(hits);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_hits = co_await hits_back.data();
    auto const got_ids = co_await ids_back.data();
    REQUIRE(got_hits.size() == grid * grid);
    REQUIRE(got_ids.size() == grid * grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const where = cc::format("cell ({}, {})", x, y);
            auto const at = y * grid + x;
            CHECK(got_ids[at][0] == (y < 4 ? 1 : 0)).context(where);
            if (y >= 4)
                continue;
            auto const is_first = x < 4;
            CHECK(got_ids[at][1] == (is_first ? 10 : 20)).context(where);
            CHECK(got_ids[at][3] == (is_first ? 0 : 1)).context(where);
            // from z = -1, to the first square at z = 1 and the second at z = 4
            CHECK(tg::abs(got_hits[at][0] - (is_first ? 2.0f : 5.0f)) < 1e-4f).context(where);
        }
    co_return;
}

ASYNC_INVOCABLE_TEST("sg - a ray through the edge two triangles share meets exactly one of them, on the GPU",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_tracing_motion_blur))
        SKIP("this context has no ray-traced motion");

    auto const pipeline = co_await shaders::software_trace::trace_shared_edge.acquire_pipeline(*ctx);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const scene = build_scene(*ctx, *cmd, sg::accel_build_flag::software_trace);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::software_trace::scene>();
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout,
                                              shaders::software_trace::scene{.world = scene.tlas->as_view(),
                                                                             .hits = hits.as_readwrite_buffer(),
                                                                             .ids = ids.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(grid, grid);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await ids_back.data();
    REQUIRE(got.size() == grid * grid);
    for (auto at = 0; at < grid * grid; ++at)
    {
        auto const where = cc::format("ray {}", at);
        CHECK(got[at][0] == 1).context(where);
        CHECK(got[at][1] == 0).context(where);
        CHECK(got[at][3] == 0).context(where);
    }
    co_return;
}

ASYNC_INVOCABLE_TEST("sg - an opted-in package's pixel stage traces what moves", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_tracing_motion_blur))
        SKIP("this context has no ray-traced motion");

    shaders::software_trace::traced_corner const corners[] = {
        {.position = tg::vec2f(-1, -1)}, {.position = tg::vec2f(1, -1)}, {.position = tg::vec2f(1, 1)},
        {.position = tg::vec2f(-1, -1)}, {.position = tg::vec2f(1, 1)},  {.position = tg::vec2f(-1, 1)},
    };
    auto const quad = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::software_trace::traced_pixels);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::software_trace::pixel_scene>();

    // Built in a list of its own, so the draw's list meets a finished TLAS at submit.
    auto build = ctx->create_command_list();
    auto const scene = build_scene(*ctx, *build, sg::accel_build_flag::software_trace);
    ctx->submit_command_list(cc::move(build));

    auto const pixels = co_await sg_test::draw_offscreen_passes(
        *ctx,
        {.width = grid,
         .height = grid,
         .colors = {sg::pixel_format::rgba32_float},
         .target_set = shaders::software_trace::traced_target::name,
         .clear_color = tg::vec4f(-2, -2, -2, -2)},
        [&](sg::command_list& cmd, sg_test::offscreen_targets const& targets)
        {
            auto const group = ctx->transient.create_binding_group(
                cmd, layout, shaders::software_trace::pixel_scene{.world = scene.tlas->as_view()});
            auto pass = cmd.raster.render_to(targets.cleared());
            pass.bind_pipeline(*pipeline);
            pass.bind_group(0, *group);
            pass.bind_vertex_buffer(quad.as_vertex_buffer());
            pass.draw({.vertex_range = {.offset = 0, .size = 6}});
        });

    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const got = pixels[0].rgba_float(x, y);
            auto const where = cc::format("pixel ({}, {})", x, y);
            if (y >= 4)
            {
                CHECK(got[3] == -1.0f).context(where);
                continue;
            }
            CHECK(got[3] == (x < 4 ? 10.0f : 20.0f)).context(where);
            CHECK(tg::abs(got[0] - (x < 4 ? 2.0f : 5.0f)) < 1e-4f).context(where);
        }
}

ASYNC_INVOCABLE_TEST("sg - a package that did not opt in is refused a tlas that moves", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->implementation_of(sg::feature::ray_query) != sg::feature_implementation::native
        || !ctx->supports(sg::feature::ray_tracing_motion_blur))
        SKIP("a native trace beside software motion is dx12's and vulkan's");
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    auto cmd = ctx->create_command_list();
    auto const scene = build_scene(*ctx, *cmd, sg::accel_build_flag::software_trace);
    auto const usage = sg::buffer_usage::readwrite_buffer;
    auto const hits = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(1), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(1), usage);

    namespace native = sg::test::sgl_shaders::raytracing;
    auto const layout = ctx->cached.acquire_binding_group_layout<native::scene>();
    auto refused = cc::string();
    try
    {
        (void)ctx->transient.create_binding_group(*cmd, layout,
                                                  native::scene{.world = scene.tlas->as_view(),
                                                                .hits = hits.as_readwrite_buffer(),
                                                                .ids = ids.as_readwrite_buffer()});
    }
    catch (sg::binding_group_exception const& e)
    {
        refused = cc::string(e.message());
    }
    CHECK(refused.contains("SOFTWARE_TRACE"));
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

// A pipeline's trace sees native motion only with the motion tags, which SGL writes into an opted-in package alone.
ASYNC_INVOCABLE_TEST("sg - a package that did not opt in is refused a tlas that moves natively",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->implementation_of(sg::feature::ray_tracing_motion_blur) != sg::feature_implementation::native)
        SKIP("a structure that moves without the software walk is built where motion is native");
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    auto cmd = ctx->create_command_list();
    auto const scene = build_scene(*ctx, *cmd, {});
    auto const usage = sg::buffer_usage::readwrite_buffer;
    auto const hits = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(1), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(1), usage);

    namespace native = sg::test::sgl_shaders::raytracing;
    auto const layout = ctx->cached.acquire_binding_group_layout<native::scene>();
    auto refused = cc::string();
    try
    {
        (void)ctx->transient.create_binding_group(*cmd, layout,
                                                  native::scene{.world = scene.tlas->as_view(),
                                                                .hits = hits.as_readwrite_buffer(),
                                                                .ids = ids.as_readwrite_buffer()});
    }
    catch (sg::binding_group_exception const& e)
    {
        refused = cc::string(e.message());
    }
    CHECK(refused.contains("SOFTWARE_TRACE"));
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - a pipeline's trace meets what moves at its ray's time, where motion is native",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline) || !ctx->supports(sg::feature::ray_tracing_motion_blur)
        || ctx->implementation_of(sg::feature::ray_tracing_motion_blur) != sg::feature_implementation::native)
        SKIP("this context traces no motion through a pipeline");
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    using pipeline_t = shaders::pipeline_motion::midway_path_t;
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(
        co_await shaders::pipeline_motion::midway_path.description(*ctx));
    REQUIRE(pipeline != nullptr);
    auto table_desc = pipeline_t::table_description(pipeline);
    auto const row = pipeline_t::add_row(table_desc, pipeline_t::hit_groups_t::midway_hits);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    auto const usage = sg::buffer_usage::accel_structure_build_input;
    auto const start = ctx->persistent.create_buffer_from_data(start_pose, usage);
    auto const end = ctx->persistent.create_buffer_from_data(end_pose, usage);
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    // the package opts in, so its inline traces could walk the scene too: the builds write the walk's structure
    auto cmd = ctx->create_command_list();
    auto triangles = sg::blas_triangles{.vertices = start.raw(), .vertex_count = 6};
    triangles.cpu_vertices = cc::as_bytes(cc::span<float const>(start_pose));
    triangles.later_poses.push_back(
        {.vertices = end.raw(), .cpu_vertices = cc::as_bytes(cc::span<float const>(end_pose))});
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1),
                                                 sg::accel_build_flag::software_trace, pipeline_t::ray_count);
    auto const offset = table->offset_of(row);
    sg::tlas_instance instances[] = {
        {.blas = blas, .instance_id = 10, .hit_group_offset = offset},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20, .hit_group_offset = offset},
    };
    instances[1].transform_at_end = cc::fixed_array<float, 12>{1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 4};
    auto const tlas = cmd->raytracing.build_tlas(instances, sg::accel_build_flag::software_trace);

    auto const group = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shaders::pipeline_motion::moving>(),
        shaders::pipeline_motion::moving{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
    auto const hits_back = cmd->download.data_from_buffer(hits);
    ctx->submit_command_list(cc::move(cmd));

    // at time 0.5 the first square stands at z = 1 and the second, whose vertices and instance both move, at z = 4
    auto const got = co_await hits_back.data();
    REQUIRE(got.size() == grid * grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const where = cc::format("cell ({}, {})", x, y);
            auto const at = y * grid + x;
            if (y >= 4)
            {
                CHECK(got[at][0] == -1.0f).context(where);
                continue;
            }
            auto const is_first = x < 4;
            CHECK(tg::abs(got[at][0] - (is_first ? 2.0f : 5.0f)) < 1e-4f).context(where);
            CHECK(got[at][1] == (is_first ? 10.0f : 20.0f)).context(where);
        }
}
