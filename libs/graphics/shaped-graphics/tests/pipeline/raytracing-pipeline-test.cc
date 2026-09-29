#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// raytracing_pipeline.sgl's `path` against ray-query-test.cc's scene: the same cells, hit through a pipeline's tables.
//
// The scene: one BLAS of two triangles over the square [0, 4]², non-opaque, so the any hits are asked.
// Triangle 0 is the half below the diagonal x + y = 4, triangle 1 the half above it, and every any hit cuts it out.
// Instance 0 (id 10) stands at z = 0, instance 1 (id 20) is moved by (4, 0, 2).
// A surface hit traces a shadow ray by (4, 0, 2), which from instance 0 lands on instance 1 and from instance 1 on
// nothing: so instance 0 is in shadow and instance 1 is lit.

namespace
{
constexpr int grid = 8;

using path_t = shaders::raytracing_pipeline_path_t;

struct expected_cell
{
    bool is_hit = false;
    int instance_id = 0;
    int primitive = 0;
    bool is_lit = false;
    float t = 0;
    float u = 0;
    float v = 0;
};

expected_cell expected_at(int x, int y)
{
    auto const px = float(x) + 0.25f;
    auto const py = float(y) + 0.5f;
    if (py >= 4)
        return {};
    auto const instance = px < 4 ? 0 : 1;
    auto const lx = instance == 0 ? px : px - 4;
    auto const primitive = lx + py < 4 ? 0 : 1;
    if (primitive == 1)
        return {};
    return {.is_hit = true,
            .instance_id = instance == 0 ? 10 : 20,
            .primitive = 0,
            .is_lit = instance == 1,
            .t = instance == 0 ? 1.0f : 3.0f,
            .u = lx / 4,
            .v = py / 4};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL ray-tracing pipeline traces two ray types, one from the other's closest hit",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");

    auto const desc = co_await shaders::raytracing_pipeline.path.description(*ctx);
    CHECK(desc.max_recursion_depth == 2u);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    auto table_desc = path_t::table_description(pipeline);
    auto const row = path_t::add_row(table_desc, path_t::hit_groups_t::textured);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    auto const input = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4f>::create_defaulted(grid * grid), usage);
    auto const ids = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec4i>::create_defaulted(grid * grid), usage);

    auto cmd = ctx->create_command_list();
    auto const triangles = sg::blas_triangles{.vertices = input.raw(), .vertex_count = 6, .is_opaque = false};
    // a record per ray type, which metal bakes into the BLAS
    auto const blas = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&triangles, 1),
                                                 sg::accel_build_flag::fast_trace, path_t::ray_count);
    auto const offset = table->offset_of(row);
    sg::tlas_instance const instances[] = {
        {.blas = blas, .instance_id = 10, .hit_group_offset = offset},
        {.blas = blas, .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 2}, .instance_id = 20, .hit_group_offset = offset},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traced>();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::traced{.world = tlas->as_view(), .hits = hits.as_readwrite_buffer(), .ids = ids.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), grid, grid);
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
            auto const want = expected_at(x, y);
            auto const at = y * grid + x;
            auto const where = cc::format("cell ({}, {})", x, y);
            CHECK(got_ids[at][0] == (want.is_hit ? 1 : 0)).context(where);
            if (!want.is_hit)
                continue;
            CHECK(got_ids[at][1] == want.instance_id).context(where);
            CHECK(got_ids[at][2] == want.primitive).context(where);
            CHECK(got_ids[at][3] == (want.is_lit ? 1 : 0)).context(where);
            CHECK(tg::abs(got_hits[at][0] - want.t) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][1] - want.u) < 1e-4f).context(where);
            CHECK(tg::abs(got_hits[at][2] - want.v) < 1e-4f).context(where);
        }
}
