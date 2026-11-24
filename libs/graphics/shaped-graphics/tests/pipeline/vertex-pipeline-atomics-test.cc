#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// vertex_pipeline_atomics.sgl's atomics in a vertex, a geometry and the tessellation stages (CHK-531).
// A native stage needs vertex_pipeline_stores; a polyfilled one is a compute pass, which needs nothing.

namespace
{
constexpr int size = 8;

/// What the stages record into: four slots each, so a stage that writes past its first shows, and 16 records.
struct traced
{
    sg::buffer<u32> count;
    sg::buffer<u32> highest;
    sg::buffer<u32> stages;
    sg::buffer<u32> records;
    sg::binding_group_handle group;
};

traced trace_buffers(sg::context& ctx)
{
    auto const zeroed = [&](isize n)
    {
        return ctx.persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(n),
                                                      sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    };
    auto result = traced{.count = zeroed(4), .highest = zeroed(4), .stages = zeroed(4), .records = zeroed(16)};
    result.group = ctx.persistent.create_binding_group(
        ctx.cached.acquire_binding_group_layout<shaders::vertex_pipeline_atomics::trace>(),
        shaders::vertex_pipeline_atomics::trace{.count = result.count.as_readwrite_buffer(),
                                                .highest = result.highest.as_readwrite_buffer(),
                                                .stages = result.stages.as_readwrite_buffer(),
                                                .records = result.records.as_readwrite_buffer()});
    return result;
}

sg_test::offscreen appended_target()
{
    return {.width = size,
            .height = size,
            .colors = {sg::pixel_format::rgba32_float},
            .target_set = shaders::vertex_pipeline_atomics::appended_target::name};
}

/// Two triangles side by side, as one draw of six vertices.
shaders::vertex_pipeline_atomics::appended_corner const k_two_triangles[] = {
    {.position = tg::vec2f(-0.9f, -0.9f)}, {.position = tg::vec2f(-0.1f, -0.9f)}, {.position = tg::vec2f(-0.5f, 0.9f)},
    {.position = tg::vec2f(0.1f, -0.9f)},  {.position = tg::vec2f(0.9f, -0.9f)},  {.position = tg::vec2f(0.5f, 0.9f)}};

/// The ways `ctx` draws a stage it has as `has_stage` says: natively, and as its polyfill where the backend draws one.
/// A native stage writes storage only with vertex_pipeline_stores; a polyfilled one is a compute pass, which always does.
cc::vector<sg::polyfill_use> uses_of(sg::context const& ctx, bool has_stage, bool polyfills)
{
    auto uses = cc::vector<sg::polyfill_use>();
    if (has_stage && ctx.supports(sg::feature::vertex_pipeline_stores))
        uses.push_back(sg::polyfill_use::never);
    if (polyfills)
        uses.push_back(sg::polyfill_use::always);
    return uses;
}

cc::string_view name_of(sg::polyfill_use use)
{
    return use == sg::polyfill_use::always ? "polyfilled" : "native";
}

/// Downloads `buffer` whole.
cc::shared_async<cc::vector<u32>> downloaded(sg::context& ctx, sg::buffer<u32> const& buffer)
{
    auto cmd = ctx.create_command_list();
    auto const back = cmd->download.data_from_buffer(buffer);
    ctx.submit_command_list(cc::move(cmd));
    auto const data = co_await back.data();
    co_return cc::vector<u32>::create_copy_of(data);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a geometry stage appends one record per primitive at the slot its atomic add claims",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const uses = uses_of(*ctx, ctx->supports(sg::feature::geometry_shader), ctx->draws_geometry_polyfill());
    if (uses.empty())
        SKIP("this device draws no geometry stage that writes storage, natively or polyfilled");

    auto const before = ctx->geometry_polyfill();
    for (auto const use : uses)
    {
        auto const by = name_of(use);
        ctx->set_geometry_polyfill(use);
        auto const trace = trace_buffers(*ctx);
        auto const corners = ctx->persistent.create_buffer_from_data(k_two_triangles, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::vertex_pipeline_atomics::appending);
        // A polyfilled stage takes the compute route, a mesh polyfill, or the mesh route, a native mesh pipeline.
        auto const built_polyfilled = pipeline->mesh_polyfill() != nullptr || pipeline->geometry_mesh_corners() > 0;
        CHECK(built_polyfilled == (use == sg::polyfill_use::always)).context(by);
        auto const pixels = co_await sg_test::draw_offscreen(*ctx, appended_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_group(0, *trace.group);
                                                                 scope.bind_vertex_buffer(corners.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                             });
        auto const count = co_await downloaded(*ctx, trace.count);
        auto const records = co_await downloaded(*ctx, trace.records);
        auto const highest = co_await downloaded(*ctx, trace.highest);
        REQUIRE(count.size() == 4);
        REQUIRE(records.size() == 16);
        REQUIRE(highest.size() == 4);

        // Vulkan runs a native geometry stage at least once per primitive, so it claims two slots or more, each holding
        // one of the two primitives; a polyfill is a compute pass of one thread per primitive, which claims exactly two
        auto const is_polyfilled = use == sg::polyfill_use::always;
        CHECK((is_polyfilled ? count[0] == 2u : count[0] >= 2u)).context(by).dump("count", count[0]);
        CHECK(count[1] == 0u).context(by);
        bool seen[2] = {false, false};
        for (auto i = isize(0); i < cc::min(isize(count[0]), records.size()); ++i)
        {
            CHECK(records[i] < 2u).context(by).dump("slot", i).dump("record", records[i]);
            if (records[i] < 2u)
                seen[records[i]] = true;
        }
        CHECK(seen[0]).context(by);
        CHECK(seen[1]).context(by);

        // a vertex stage may run more than once per vertex, and its max holds however often it ran
        CHECK(highest[0] == 5u).context(by);
        CHECK(highest[1] == 0u).context(by);

        // and the triangles still reached the target
        CHECK(pixels[0].rgba_float(1, size / 2) == tg::vec4f(1, 1, 1, 1)).context(by);
        CHECK(pixels[0].rgba_float(size - 2, size / 2) == tg::vec4f(1, 1, 1, 1)).context(by);
    }
    ctx->set_geometry_polyfill(before);
}

ASYNC_INVOCABLE_TEST("sg - a tessellation control and evaluation stage each update an atomic",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const uses = uses_of(*ctx, ctx->supports(sg::feature::tessellation_shader), ctx->draws_tessellation_polyfill());
    if (uses.empty())
        SKIP("this device draws no tessellation stage that writes storage, natively or polyfilled");

    auto const before = ctx->tessellation_polyfill();
    for (auto const use : uses)
    {
        auto const by = name_of(use);
        ctx->set_tessellation_polyfill(use);
        auto const trace = trace_buffers(*ctx);
        auto const corners = ctx->persistent.create_buffer_from_data(k_two_triangles, sg::buffer_usage::vertex_buffer);
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::vertex_pipeline_atomics::marking);
        CHECK((pipeline->tessellation_polyfill() != nullptr) == (use == sg::polyfill_use::always)).context(by);
        auto const pixels = co_await sg_test::draw_offscreen(*ctx, appended_target(),
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.bind_group(0, *trace.group);
                                                                 scope.bind_vertex_buffer(corners.as_vertex_buffer());
                                                                 scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                             });
        auto const stages = co_await downloaded(*ctx, trace.stages);
        REQUIRE(stages.size() == 4);
        // each stage set its bit, and wrote nowhere else
        CHECK(stages[0] == 3u).context(by);
        CHECK(stages[1] == 0u).context(by);
        CHECK(pixels[0].rgba_float(1, size / 2) == tg::vec4f(1, 1, 1, 1)).context(by);
    }
    ctx->set_tessellation_polyfill(before);
}
