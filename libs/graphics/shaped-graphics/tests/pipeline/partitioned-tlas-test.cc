#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// A partitioned tlas traced by raytracing.sgl's inline trace, built and then updated in place.
//
// The scene is ray-query-test.cc's: one BLAS of two triangles over the square [0, 4]², each thread tracing one ray
// along +z from z = -1 through its cell of an 8 × 8 grid, a quarter cell off the centre in x.
// Instance 0 (id 10) stands at z = 0 in partition 0; instance 1 (id 20) stands at x + 4, z = 2, in the global one.

namespace
{
constexpr int grid = 8;

/// Where one instance of the square stands, as the grid sees it.
struct placed
{
    int instance_id = 0;
    int slot = 0;
    float x = 0;
    float z = 0;
};

/// What the ray through cell (x, y) hits among `instances`, the nearest first; `none` where it hits nothing.
struct expected_cell
{
    bool is_hit = false;
    int instance_id = 0;
    int slot = 0;
    float t = 0;
};

expected_cell expected_at(int x, int y, cc::span<placed const> instances)
{
    auto const px = float(x) + 0.25f;
    auto const py = float(y) + 0.5f;
    auto best = expected_cell();
    if (py >= 4)
        return best;
    for (auto const& p : instances)
        if (px >= p.x && px < p.x + 4)
        {
            auto const t = p.z + 1;
            if (!best.is_hit || t < best.t)
                best = {.is_hit = true, .instance_id = p.instance_id, .slot = p.slot, .t = t};
        }
    return best;
}

/// Traces the grid against `world` and checks every cell against `instances`.
cc::shared_async<cc::unit> trace_and_check(sg::context_handle ctx,
                                           sg::compute_pipeline_handle pipeline,
                                           sg::tlas_handle world,
                                           cc::vector<placed> instances,
                                           cc::string what)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::raytracing::scene>();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::raytracing::scene{.world = world->as_view(),
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
            auto const want = expected_at(x, y, instances);
            auto const at = y * grid + x;
            auto const where = cc::format("{}: cell ({}, {})", what, x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (!want.is_hit)
                continue;
            CHECK(got_ids[at][1] == want.instance_id).context(where);
            // a partitioned tlas numbers its instances by slot
            CHECK(got_ids[at][3] == want.slot).context(where);
            CHECK(tg::abs(got_hits[at][0] - want.t) < 1e-4f).context(where);
        }
    co_return;
}

/// Traces the grid against `world`, a square of two clusters, and checks each hit reads the cluster it is in:
/// 7 below its diagonal and 9 above it.
cc::shared_async<cc::unit> check_cluster_ids(sg::context_handle ctx, sg::tlas_handle world)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);
    auto const clustered = co_await shaders::raytracing::trace_cells_clusters.acquire_pipeline(*ctx);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);
    auto cmd = ctx->create_command_list();
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::raytracing::scene>();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::raytracing::scene{.world = world->as_view(),
                                                                                      .hits = hits.as_readwrite_buffer(),
                                                                                      .ids = ids.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*clustered);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(grid, grid);
    auto const ids_back = cmd->download.data_from_buffer(ids);
    ctx->submit_command_list(cc::move(cmd));
    auto const got = co_await ids_back.data();
    REQUIRE(got.size() == grid * grid);
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            auto const is_below = float(x) + 0.25f + float(y) + 0.5f < 4.0f;
            CHECK(got[y * grid + x][2] == (is_below ? 7 : 9)).context(cc::format("cell ({}, {})", x, y));
        }
    co_return;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a partitioned tlas traces as a tlas does, and an update in place moves what it holds",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::partitioned_tlas))
        SKIP("this device has no partitioned tlas");
    auto const pipeline = co_await shaders::raytracing::trace_cells_opaque.acquire_pipeline(*ctx);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const ptlas = ctx->persistent.create_partitioned_tlas({.instance_capacity = 3,
                                                                .partition_count = 1,
                                                                .max_instances_per_partition = 2,
                                                                .max_global_instances = 1,
                                                                .allows_partition_translation = true});
    REQUIRE(ptlas != nullptr);
    CHECK(!ptlas->has_been_updated());

    auto build = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6};
    auto const blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1));
    sg::ptlas_write const writes[] = {
        {.index = 0, .partition = 0, .instance = {.blas = blas, .instance_id = 10}},
        {.index = 1,
         .partition = sg::global_partition,
         .instance = {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20}},
    };
    build->raytracing.update(*ptlas, {.writes = writes});
    ctx->submit_command_list(cc::move(build));
    CHECK(ptlas->has_been_updated());

    co_await trace_and_check(
        ctx, pipeline, ptlas,
        {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}, {.instance_id = 20, .slot = 1, .x = 4, .z = 2}}, "built");

    // A write moves instance 1 over instance 0, now in front of it; slot 2, never written, stays empty.
    auto moved = ctx->create_command_list();
    sg::ptlas_write const over[] = {
        {.index = 1,
         .partition = sg::global_partition,
         .instance = {.blas = blas, .transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -0.5f}, .instance_id = 20}},
    };
    moved->raytracing.update(*ptlas, {.writes = over});
    ctx->submit_command_list(cc::move(moved));
    co_await trace_and_check(
        ctx, pipeline, ptlas,
        {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}, {.instance_id = 20, .slot = 1, .x = 0, .z = -0.5f}},
        "written over");

    // Partition 0 moves by 4 in x, which carries instance 0 without writing it; clearing slot 1 leaves it alone.
    auto translated = ctx->create_command_list();
    sg::ptlas_translation const translations[] = {{.partition = 0, .translation = {4, 0, 0}}};
    int const clears[] = {1};
    translated->raytracing.update(*ptlas, {.clears = clears, .translations = translations});
    ctx->submit_command_list(cc::move(translated));
    co_await trace_and_check(ctx, pipeline, ptlas, {{.instance_id = 10, .slot = 0, .x = 4, .z = 0}},
                             "translated and cleared");
}

ASYNC_INVOCABLE_TEST("sg - a partitioned tlas updates from records a compute pass wrote, which sg counts as written",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::partitioned_tlas))
        SKIP("this device has no partitioned tlas");
    auto const pipeline = co_await shaders::raytracing::trace_cells_opaque.acquire_pipeline(*ctx);
    auto const write_instances = co_await shaders::ptlas_records::write_instances.acquire_pipeline(*ctx);
    auto const translate_partition = co_await shaders::ptlas_records::translate_partition.acquire_pipeline(*ctx);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const ptlas = ctx->persistent.create_partitioned_tlas({.instance_capacity = 3,
                                                                .partition_count = 1,
                                                                .max_instances_per_partition = 2,
                                                                .max_global_instances = 1,
                                                                .allows_partition_translation = true});

    auto build = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6};
    auto const blas = build->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1));
    ctx->submit_command_list(cc::move(build));
    sg::blas_handle const listed[] = {blas};
    auto const table = ctx->persistent.create_blas_table(listed);
    CHECK(table->size() == 1);

    // room for more records than the shaders write, so only the counts they bump say how many there are
    auto const usage = sg::buffer_usage::readwrite_buffer;
    auto const writes = ctx->persistent.create_buffer<sg::gpu_ptlas_write>(4, usage);
    auto const translations = ctx->persistent.create_buffer<sg::gpu_ptlas_translation>(4, usage);
    auto const counts
        = ctx->persistent.create_buffer_from_data(cc::vector<u32>{0, 0}, usage | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::ptlas_records::records>();
    auto const group
        = ctx->persistent.create_binding_group(layout, shaders::ptlas_records::records{
                                                           .blases = table->view(),
                                                           .writes = writes.as_readwrite_buffer(),
                                                           .translations = translations.as_readwrite_buffer(),
                                                           .counts = counts.as_readwrite_buffer(),
                                                       });
    sg::blas_table_handle const tables[] = {table};

    auto written = ctx->create_command_list();
    written->compute.bind_pipeline(*write_instances);
    written->compute.bind_group(0, *group);
    written->compute.dispatch_threads(2);
    written->raytracing.update_indirect(
        *ptlas, {.writes = {.records = writes.raw(), .max_count = 4, .count = counts.raw(), .count_offset_in_bytes = 0},
                 .tables = tables});
    ctx->submit_command_list(cc::move(written));
    CHECK(ptlas->has_been_updated());
    CHECK(ptlas->is_gpu_written());
    co_await trace_and_check(
        ctx, pipeline, ptlas,
        {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}, {.instance_id = 20, .slot = 1, .x = 4, .z = 2}}, "written");

    auto translated = ctx->create_command_list();
    translated->compute.bind_pipeline(*translate_partition);
    translated->compute.bind_group(0, *group);
    translated->compute.dispatch_threads(1);
    translated->raytracing.update_indirect(
        *ptlas, {.translations
                 = {.records = translations.raw(), .max_count = 4, .count = counts.raw(), .count_offset_in_bytes = 4},
                 .tables = tables});
    ctx->submit_command_list(cc::move(translated));
    co_await trace_and_check(
        ctx, pipeline, ptlas,
        {{.instance_id = 10, .slot = 0, .x = 4, .z = 0}, {.instance_id = 20, .slot = 1, .x = 4, .z = 2}}, "translated");
}

ASYNC_INVOCABLE_TEST("sg - a cluster BLAS traces as a blas does, through a tlas and a partitioned tlas",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::clustered_geometry))
        SKIP("this device has no clustered geometry");
    auto const pipeline = co_await shaders::raytracing::trace_cells_opaque.acquire_pipeline(*ctx);

    // the square as two clusters, a triangle each: 7 below its diagonal, 9 above it
    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    u32 const indices[] = {0, 1, 2};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const index_input
        = ctx->persistent.create_buffer_from_data(indices, sg::buffer_usage::accel_structure_build_input);

    auto build = ctx->create_command_list();
    sg::cluster_triangles const square[] = {
        {.vertices = input.raw(), .vertex_count = 3, .indices = index_input.raw(), .triangle_count = 1, .cluster_id = 7},
        {.vertices = input.raw(),
         .vertex_count = 3,
         .vertex_offset_in_bytes = 36,
         .indices = index_input.raw(),
         .triangle_count = 1,
         .cluster_id = 9},
    };
    auto const clusters = build->raytracing.build_clusters(square);
    REQUIRE(clusters != nullptr);
    CHECK(clusters->cluster_count() == 2);
    sg::cluster_range const whole[] = {{.first = 0, .count = 2}};
    auto const blases = build->raytracing.build_cluster_blas(clusters, whole);
    REQUIRE(blases.size() == 1);
    CHECK(blases[0]->geometry_count() == 1);
    sg::tlas_instance const placed[] = {{.blas = blases[0], .instance_id = 10}};
    auto const world = build->raytracing.build_tlas(placed);
    ctx->submit_command_list(cc::move(build));
    co_await trace_and_check(ctx, pipeline, world, {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}}, "a tlas");

    co_await check_cluster_ids(ctx, world);

    if (!ctx->supports(sg::feature::partitioned_tlas))
        co_return;
    auto const ptlas = ctx->persistent.create_partitioned_tlas(
        {.instance_capacity = 2, .partition_count = 1, .max_instances_per_partition = 2, .max_global_instances = 1});
    auto written = ctx->create_command_list();
    sg::ptlas_write const writes[] = {
        {.index = 0, .partition = 0, .instance = {.blas = blases[0], .instance_id = 10}},
        {.index = 1,
         .partition = sg::global_partition,
         .instance = {.blas = blases[0], .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20}},
    };
    written->raytracing.update(*ptlas, {.writes = writes});
    ctx->submit_command_list(cc::move(written));
    co_await trace_and_check(
        ctx, pipeline, ptlas,
        {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}, {.instance_id = 20, .slot = 1, .x = 4, .z = 2}},
        "a partitioned tlas");
}

ASYNC_INVOCABLE_TEST("sg - a cluster pool is built in place from records a compute pass wrote, and its old BLASes go "
                     "stale",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::clustered_geometry))
        SKIP("this device has no clustered geometry");
    auto const pipeline = co_await shaders::raytracing::trace_cells_opaque.acquire_pipeline(*ctx);
    auto const write_clusters = co_await shaders::ptlas_records::write_clusters.acquire_pipeline(*ctx);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    u32 const indices[] = {0, 1, 2};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const index_input
        = ctx->persistent.create_buffer_from_data(indices, sg::buffer_usage::accel_structure_build_input);
    sg::raw_buffer_handle const listed[] = {input.raw(), index_input.raw()};
    auto const buffers = ctx->persistent.create_buffer_table(listed);
    CHECK(buffers->size() == 2);

    // room for more clusters than the shader writes, so only the count it bumps says how many there are
    auto const pool = ctx->persistent.create_cluster_pool({.max_clusters = 4,
                                                           .max_triangles_per_cluster = 1,
                                                           .max_vertices_per_cluster = 3,
                                                           .max_total_triangles = 4,
                                                           .max_total_vertices = 12});
    REQUIRE(pool != nullptr);
    CHECK(pool->generation() == 0);
    // the build reads both as its input
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::accel_structure_build_input;
    auto const records = ctx->persistent.create_buffer<sg::gpu_clas_triangles>(4, usage);
    auto const counts = ctx->persistent.create_buffer_from_data(cc::vector<u32>{0}, usage | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::ptlas_records::cluster_records>();
    auto const group = ctx->persistent.create_binding_group(
        layout, shaders::ptlas_records::cluster_records{.buffers = buffers->view(),
                                                        .clusters = records.as_readwrite_buffer(),
                                                        .counts = counts.as_readwrite_buffer()});
    sg::buffer_table_handle const tables[] = {buffers};

    auto build = ctx->create_command_list();
    build->compute.bind_pipeline(*write_clusters);
    build->compute.bind_group(0, *group);
    build->compute.dispatch_threads(2);
    build->raytracing.build_clusters_indirect(
        *pool, {.clusters = {.records = records.raw(), .max_count = 4, .count = counts.raw()}, .tables = tables});
    CHECK(pool->generation() == 1);
    sg::cluster_range const whole[] = {{.first = 0, .count = 2}};
    auto const blases = build->raytracing.build_cluster_blas(pool, whole);
    REQUIRE(blases.size() == 1);
    CHECK(!blases[0]->is_stale());
    sg::tlas_instance const placed[] = {{.blas = blases[0], .instance_id = 10}};
    auto const world = build->raytracing.build_tlas(placed);
    ctx->submit_command_list(cc::move(build));
    co_await trace_and_check(ctx, pipeline, world, {{.instance_id = 10, .slot = 0, .x = 0, .z = 0}}, "a pool");
    co_await check_cluster_ids(ctx, world);

    // a second build replaces every cluster, so a BLAS of the first lists clusters that are gone
    auto again = ctx->create_command_list();
    again->raytracing.build_clusters_indirect(
        *pool, {.clusters = {.records = records.raw(), .max_count = 4, .count = counts.raw()}, .tables = tables});
    ctx->submit_command_list(cc::move(again));
    co_await ctx->idle_completion();
    CHECK(pool->generation() == 2);
    CHECK(blases[0]->is_stale());

    // a tlas built before the rebuild still lists that BLAS, so a dispatch tracing it would meet the gone clusters
    if (!ctx->hit_record_checks())
        co_return;
    auto const usage_back = sg::buffer_usage::readwrite_buffer;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage_back);
    auto const ids
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage_back);
    auto stale = ctx->create_command_list();
    auto const scene_layout = ctx->cached.acquire_binding_group_layout<shaders::raytracing::scene>();
    auto const scene = ctx->transient.create_binding_group(*stale, scene_layout,
                                                           shaders::raytracing::scene{.world = world->as_view(),
                                                                                      .hits = hits.as_readwrite_buffer(),
                                                                                      .ids = ids.as_readwrite_buffer()});
    stale->compute.bind_pipeline(*pipeline);
    stale->compute.bind_group(0, *scene);
    CHECK_ASSERTS(stale->compute.dispatch_threads(grid, grid));
    ctx->submit_command_list(cc::move(stale));
    co_await ctx->idle_completion();
}
