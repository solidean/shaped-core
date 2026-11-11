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
} // namespace

ASYNC_INVOCABLE_TEST("sg - a partitioned tlas traces as a tlas does, and an update in place moves what it holds",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::partitioned_tlas))
        SKIP("this device has no partitioned tlas");
    // vulkan reaches one by its address alone, which only SGL's own SPIR-V traces by
    if (ctx->backend() == sg::backend_kind::vulkan && !ctx->prefers_native_spirv())
        SKIP("this context takes DXC's SPIR-V, which traces a structure by its descriptor");
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
