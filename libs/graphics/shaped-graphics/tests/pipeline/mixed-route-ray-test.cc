#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/float.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

using namespace cc::primitive_defines;

// Every stage of a ray-tracing pipeline through SGL's own binary, in each pairing with the same stage through DXC.

namespace
{
constexpr int k_grid = 8;
constexpr int k_cell_words = 8;
constexpr float k_tolerance = 1e-4f;

namespace text = sg::test::mixed_text::ray_stages;
namespace dxil = sg::test::mixed_dxil::ray_stages;
namespace spirv = sg::test::mixed_spirv::ray_stages;

/// Which stages of a description come from the native route.
enum route_mask : u32
{
    k_raygen = 1,
    k_miss = 2,
    k_hit = 4,
    k_callable = 8,
};

sg::raytracing_pipeline_description mixed(sg::raytracing_pipeline_description const& dxc,
                                          sg::raytracing_pipeline_description const& native,
                                          u32 mask)
{
    auto result = dxc;
    if (mask & k_raygen)
        result.raygen_shaders = native.raygen_shaders;
    if (mask & k_miss)
        result.miss_shaders = native.miss_shaders;
    if (mask & k_hit)
        result.hit_shaders = native.hit_shaders;
    if (mask & k_callable)
        result.callable_shaders = native.callable_shaders;
    return result;
}

template <class pipeline_t>
cc::shared_async<cc::vector<tg::vec4f>> traced(sg::context_handle ctx, sg::raytracing_pipeline_description desc)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);
    auto const pipeline = co_await ctx->cached.acquire_raytracing_pipeline(desc);
    REQUIRE(pipeline != nullptr);
    auto table_desc = pipeline_t::table_description(pipeline);
    auto const squares = pipeline_t::add_row(table_desc, pipeline_t::hit_groups_t::squares);
    auto const balls = pipeline_t::add_row(table_desc, pipeline_t::hit_groups_t::balls);
    auto const table = ctx->uncached.create_raytracing_shader_table(table_desc);
    REQUIRE(table != nullptr);

    float const vertices[] = {0, 0, 0, 4, 0, 0, 0, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0};
    float const boxes[] = {0, 0, 0, 2, 2, 2, 2, 0, 0, 4, 2, 2};
    auto const triangle_input
        = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::accel_structure_build_input);
    auto const box_input = ctx->persistent.create_buffer_from_data(boxes, sg::buffer_usage::accel_structure_build_input);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const cells = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(k_grid * k_grid * k_cell_words), usage);

    auto cmd = ctx->create_command_list();
    auto const square_input = sg::blas_triangles{.vertices = triangle_input.raw(), .vertex_count = 6, .is_opaque = false};
    auto const ball_input = sg::blas_aabbs{.aabbs = box_input.raw(), .aabb_count = 2, .is_opaque = false};
    auto const square = cmd->raytracing.build_blas(cc::span<sg::blas_triangles const>(&square_input, 1),
                                                   sg::accel_build_flag::fast_trace, pipeline_t::ray_count);
    auto const ball = cmd->raytracing.build_blas(cc::span<sg::blas_aabbs const>(&ball_input, 1),
                                                 sg::accel_build_flag::fast_trace, pipeline_t::ray_count);
    sg::tlas_instance const instances[] = {
        {.blas = square, .instance_id = 40, .hit_group_offset = table->offset_of(squares)},
        {.blas = ball,
         .transform = {1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 0},
         .instance_id = 50,
         .hit_group_offset = table->offset_of(balls)},
        // a quarter turn about y, so the square faces the other way and every row of the transform is its own
        {.blas = square,
         .transform = {0, 0, 2, 5, 0, 1, 0, 0, -2, 0, 0, 6},
         .instance_id = 77,
         .hit_group_offset = table->offset_of(squares)},
    };
    auto const tlas = cmd->raytracing.build_tlas(instances);

    auto const layout = ctx->cached.acquire_binding_group_layout<text::ray_scene>();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, text::ray_scene{.world = tlas->as_view(), .cells = cells.as_readwrite_buffer()});
    cmd->raytracing.bind_pipeline(*pipeline);
    cmd->raytracing.bind_group(0, *group);
    cmd->raytracing.dispatch_rays(*table, sg::raygen_index(0), k_grid, k_grid);
    auto const back = cmd->download.data_from_buffer(cells);
    ctx->submit_command_list(cc::move(cmd));
    auto const words = co_await back.data();
    co_return cc::vector<tg::vec4f>::create_copy_of(words);
}

void check_same(cc::span<tg::vec4f const> got, cc::span<tg::vec4f const> reference, u32 mask)
{
    REQUIRE(got.size() == reference.size());
    for (auto i = isize(0); i < got.size(); ++i)
        for (auto lane = 0; lane < 4; ++lane)
        {
            auto const a = got[i][lane];
            auto const b = reference[i][lane];
            auto const are_alike = a == b || (cc::is_nan(a) && cc::is_nan(b)) || tg::abs(a - b) < k_tolerance;
            CHECK(are_alike).context(cc::format("native stages {}: word {} lane {}", mask, i, lane));
        }
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - each stage of a ray-tracing pipeline through SGL's binary traces what DXC's traces",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::raytracing_pipeline))
        SKIP("this device has no ray-tracing pipelines");
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to mix with");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (!takes_dxil && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("this context takes neither DXIL nor SPIR-V");

    auto const dxc = co_await text::every_stage.description(*ctx);
    auto const native
        = takes_dxil ? co_await dxil::every_stage.description(*ctx) : co_await spirv::every_stage.description(*ctx);
    REQUIRE(!dxc.raygen_shaders.empty());
    REQUIRE(!native.raygen_shaders.empty());
    CHECK(dxc.raygen_shaders[0].compiler.name != "sgl");
    CHECK(native.raygen_shaders[0].compiler.name == "sgl");
    CHECK(native.miss_shaders[0].compiler.name == "sgl");
    CHECK(native.callable_shaders[0].compiler.name == "sgl");
    auto stages = 0;
    for (auto const& group : native.hit_shaders)
        for (auto const& stage : {group.closest_hit, group.any_hit, group.intersection})
            if (stage.has_value())
            {
                CHECK(stage.value().compiler.name == "sgl");
                ++stages;
            }
    CHECK(stages >= 5);

    auto const reference = co_await traced<text::every_stage_t>(ctx, dxc);
    // the scene answers: a test that compares two blanks says nothing
    int kinds[2] = {};
    for (auto i = 0; i < k_grid * k_grid; ++i)
        ++kinds[reference[i * k_cell_words + 1][0] == 50.0f ? 1 : 0];
    CHECK(kinds[0] > 0);
    CHECK(kinds[1] > 0);
    for (auto mask = u32(1); mask < 16; ++mask)
        check_same(co_await traced<text::every_stage_t>(ctx, mixed(dxc, native, mask)), reference, mask);
}

// EMIT-246: a regrouped trace and a hit object through SGL's binary, on the one route that has them on this machine.
ASYNC_INVOCABLE_TEST("sg - a hit object and a regrouped trace through SGL's binary trace what DXC's trace",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_execution_reordering))
        SKIP("this device has no shader execution reordering");
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to mix with");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (takes_dxil)
        SKIP("a regrouped stage holds a body in a target's own text, which the DXIL route does not lower yet");

    auto const regrouped_dxc = co_await text::regrouped_stage.description(*ctx);
    auto const regrouped_native = takes_dxil ? co_await dxil::regrouped_stage.description(*ctx)
                                             : co_await spirv::regrouped_stage.description(*ctx);
    CHECK(regrouped_dxc.raygen_shaders[0].compiler.name != "sgl");
    CHECK(regrouped_native.raygen_shaders[0].compiler.name == "sgl");
    auto const regrouped_reference = co_await traced<text::regrouped_stage_t>(ctx, regrouped_dxc);
    check_same(co_await traced<text::regrouped_stage_t>(ctx, mixed(regrouped_dxc, regrouped_native, k_raygen)),
               regrouped_reference, k_raygen);
    check_same(co_await traced<text::regrouped_stage_t>(ctx, regrouped_native), regrouped_reference, 15);

    auto const inspected_dxc = co_await text::inspected_stage.description(*ctx);
    auto const inspected_native = takes_dxil ? co_await dxil::inspected_stage.description(*ctx)
                                             : co_await spirv::inspected_stage.description(*ctx);
    CHECK(inspected_native.raygen_shaders[0].compiler.name == "sgl");
    auto const inspected_reference = co_await traced<text::inspected_stage_t>(ctx, inspected_dxc);
    check_same(co_await traced<text::inspected_stage_t>(ctx, mixed(inspected_dxc, inspected_native, k_raygen)),
               inspected_reference, k_raygen);
    check_same(co_await traced<text::inspected_stage_t>(ctx, inspected_native), inspected_reference, 15);

    // the hit objects answered: a test that compares two blanks says nothing
    auto hits = 0;
    for (auto i = 0; i < k_grid * k_grid; ++i)
        hits += inspected_reference[i * k_cell_words + 2][0] == 1.0f ? 1 : 0;
    CHECK(hits > 0);
}
