#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// raytracing.sgl's inline traces against one known scene, on every backend: natively, or through webgpu's polyfill.
//
// The scene: one BLAS of two triangles over the square [0, 4]², non-opaque, so the any-hit decision is asked.
// Triangle 0 is the half below the diagonal x + y = 4, triangle 1 the half above it.
// Instance 0 (id 10) stands at z = 0, instance 1 (id 20) is moved by (4, 0, 2).
// Each thread traces one ray along +z from z = -1 through its cell, a quarter cell off the centre in x.

namespace
{
constexpr int grid = 8;

struct expected_cell
{
    bool is_hit = false;
    int instance_id = 0;
    int instance_index = 0;
    int primitive = 0;
    float t = 0;
    float u = 0;
    float v = 0;
};

/// What a ray through cell (x, y) hits, where the cut-out triangle is `is_cut` out.
expected_cell expected_at(int x, int y, bool is_cut)
{
    auto const px = float(x) + 0.25f;
    auto const py = float(y) + 0.5f;
    if (py >= 4)
        return {};
    auto const instance = px < 4 ? 0 : 1;
    auto const lx = instance == 0 ? px : px - 4;
    auto const primitive = lx + py < 4 ? 0 : 1;
    if (primitive == 1 && is_cut)
        return {};
    // triangle 0 is (0,0) (4,0) (0,4), triangle 1 is (4,0) (4,4) (0,4): u weighs the second vertex, v the third
    auto const u = primitive == 0 ? lx / 4 : (lx + py) / 4 - 1;
    auto const v = primitive == 0 ? py / 4 : 1 - lx / 4;
    return {.is_hit = true,
            .instance_id = instance == 0 ? 10 : 20,
            .instance_index = instance,
            .primitive = primitive,
            .t = instance == 0 ? 1.0f : 3.0f,
            .u = u,
            .v = v};
}

/// Builds the scene, runs `pipeline` over the grid and checks every cell.
cc::shared_async<cc::unit> trace_grid(sg::context_handle ctx, sg::compute_pipeline_handle pipeline, bool is_cut)
{
    // two triangles, non-indexed, three floats per vertex
    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6, .is_opaque = false};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1));
    sg::tlas_instance const instances[] = {
        {.blas = blas, .instance_id = 10},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::scene>();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::scene{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer(), .ids = ids.as_readwrite_buffer()});
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
            auto const want = expected_at(x, y, is_cut);
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (!want.is_hit)
                continue;
            CHECK(got_ids[at][1] == want.instance_id).context(where);
            CHECK(got_ids[at][2] == want.primitive).context(where);
            CHECK(got_ids[at][3] == want.instance_index).context(where);
            // hardware and the polyfill agree on t and the barycentrics within rounding, never bit for bit
            CHECK(tg::abs(got_hits[at][0] - want.t) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][1] - want.u) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][2] - want.v) < 1e-4f).context(where);
        }
    co_return;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an inline trace finds the nearest triangle, and the any-hit decision cuts one out",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_query))
        SKIP("this device has no ray queries");
    auto const pipeline = co_await shaders::raytracing.trace_cells.acquire_pipeline(*ctx);
    co_await trace_grid(ctx, pipeline, true);
}

ASYNC_INVOCABLE_TEST("sg - an inline trace forced opaque asks no any-hit decision", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_query))
        SKIP("this device has no ray queries");
    auto const pipeline = co_await shaders::raytracing.trace_cells_opaque.acquire_pipeline(*ctx);
    co_await trace_grid(ctx, pipeline, false);
}

// raytracing.sgl's sphere traces: a procedural BLAS of two boxes, each holding the unit sphere at its centre.
// Box k spans x from 2k to 2k + 2 and y and z from 0 to 2; one instance, id 30, stands where the boxes were built.
// Each thread traces one ray along +z from z = -1 through its cell of an 8 × 8 grid over [0, 4]².

namespace
{
struct expected_sphere
{
    bool is_hit = false;
    int primitive = 0;
    float t = 0;
    tg::vec3f normal;
};

expected_sphere expected_sphere_at(int x, int y, bool is_cut)
{
    auto const px = 0.5f * float(x) + 0.25f;
    auto const py = 0.5f * float(y) + 0.25f;
    auto const primitive = px < 2 ? 0 : 1;
    auto const dx = px - float(2 * primitive + 1);
    auto const dy = py - 1;
    auto const distance2 = dx * dx + dy * dy;
    if (distance2 >= 1 || (primitive == 1 && is_cut))
        return {};
    auto const dz = tg::sqrt(1 - distance2);
    return {.is_hit = true, .primitive = primitive, .t = 2 - dz, .normal = tg::vec3f(dx, dy, -dz)};
}

cc::shared_async<cc::unit> trace_sphere_grid(sg::context_handle ctx, sg::compute_pipeline_handle pipeline, bool is_cut)
{
    float const boxes[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 4, 2, 2};
    auto const input = ctx->persistent.create_buffer_from_data(boxes, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const aabbs = sg::blas_aabbs{.aabbs = input.raw(), .aabb_count = 2, .is_opaque = false};
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&aabbs, 1));
    sg::tlas_instance const instances[] = {{.blas = blas, .instance_id = 30}};
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sphere_scene>();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::sphere_scene{.world = tlas->as_view(),
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
            auto const want = expected_sphere_at(x, y, is_cut);
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (!want.is_hit)
                continue;
            CHECK(got_ids[at][1] == 30).context(where);
            CHECK(got_ids[at][2] == want.primitive).context(where);
            CHECK(got_ids[at][3] == 0).context(where);
            CHECK(tg::abs(got_hits[at][0] - want.t) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][1] - want.normal[0]) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][2] - want.normal[1]) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][3] - want.normal[2]) < 1e-4f).context(where);
        }
    co_return;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an inline trace asks the intersection of each box, and the any-hit decision cuts a report "
                     "out",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_query))
        SKIP("this device has no ray queries");
    auto const pipeline = co_await shaders::raytracing.trace_spheres.acquire_pipeline(*ctx);
    co_await trace_sphere_grid(ctx, pipeline, true);
}

ASYNC_INVOCABLE_TEST("sg - an inline trace without a decision accepts every report", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::ray_query))
        SKIP("this device has no ray queries");
    auto const pipeline = co_await shaders::raytracing.trace_spheres_all.acquire_pipeline(*ctx);
    co_await trace_sphere_grid(ctx, pipeline, false);
}
