#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// tests/shaders/sgl/meshes.sgl's mesh and task stages, drawn where the device has them, on every backend SGL reaches.
// Every probe is a pixel centre, and no edge of any triangle passes through one.

namespace
{
constexpr int size = 8;

sg_test::offscreen mesh_target()
{
    return {.width = size, .height = size, .colors = {sg::pixel_format::rgba8_unorm}};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a mesh-shaded pipeline is refused without the feature, or beside a vertex input",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    // the frontend refuses before compiling anything, so a stand-in shader of no bytecode is enough
    auto desc = sg::raster_pipeline_description{
        .layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{}),
        .mesh_shader = sg::compiled_shader{.stage = sg::shader_stage::mesh, .entry_point = "ms"},
    };
    desc.vertex_input.slots.push_back({.stride = 16});
    auto const refused = ctx->uncached.try_create_raster_pipeline(desc);
    REQUIRE(refused.has_error());
    if (ctx->supports(sg::feature::mesh_shader))
        CHECK(refused.error().to_string().contains("vertex input")).dump("error", refused.error().to_string());
    else
        CHECK(refused.error().to_string().contains("sg::feature::mesh_shader")).dump("error", refused.error().to_string());
    co_return;
}

ASYNC_INVOCABLE_TEST("sg - an SGL mesh stage writes the triangle its pipeline draws (CHK-480)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::covered);
    CHECK(pipeline->is_mesh_shaded());
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(0, 255, 0, 255)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - two SGL mesh workgroups each write their half, by their @workgroup_id",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::split);
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(2);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const red = x < size / 2 ? 0 : 255;
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(red, 0, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - an SGL task stage launches two mesh workgroups and hands them its payload (CHK-485)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::launched);
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    // one task workgroup, two mesh workgroups: red by the mesh workgroup's index, blue from the payload
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const red = x < size / 2 ? 0 : 255;
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(red, 0, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
        }
}

namespace
{
/// The image `culled` draws in four task workgroups: columns 2, 4 and 5 alone, by the task workgroup and the launch.
tg::vec4i culled_pixel(int x)
{
    if (x == 2)
        return tg::vec4i(64, 0, 255, 255);
    if (x == 4)
        return tg::vec4i(128, 0, 255, 255);
    if (x == 5)
        return tg::vec4i(128, 255, 255, 255);
    return tg::vec4i(0, 0, 0, 0);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL task workgroup that launches no mesh workgroup draws nothing, beside ones that do",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::culled);
    REQUIRE(pipeline->mesh_polyfill() == nullptr);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(4);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == culled_pixel(x)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - an SGL pixel stage reads what its mesh stage wrote per triangle (CHK-489)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::tinted);
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    // triangle 0 is below the diagonal from the bottom-left corner to the top-right one, red, and triangle 1 above, blue;
    // a pixel on the diagonal is either's
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            if (x + y == size - 1)
                continue;
            auto const expected = x + y > size - 1 ? tg::vec4i(255, 0, 0, 255) : tg::vec4i(0, 0, 255, 255);
            CHECK(pixels[0].rgba8(x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - an SGL mesh stage of lines draws lines, which its pipeline rasterizes as lines",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::crossed);
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw_mesh_tasks(1);
                                                         });
    // along each line, away from its ends and from where the two cross; and a pixel on neither
    for (auto x = 1; x < size - 1; ++x)
        if (x != 5)
            CHECK(pixels[0].rgba8(x, 3) == tg::vec4i(255, 0, 0, 255)).context(cc::format("row 3, pixel {}", x));
    for (auto y = 1; y < size - 1; ++y)
        if (y != 3)
            CHECK(pixels[0].rgba8(5, y) == tg::vec4i(0, 255, 0, 255)).context(cc::format("column 5, pixel {}", y));
    CHECK(pixels[0].rgba8(1, 6) == tg::vec4i(0, 0, 0, 0));
}

ASYNC_INVOCABLE_TEST("sg - an indirect mesh draw reads its workgroup counts from a buffer, a record at a time",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::split);
    // both halves; then the left alone, beside a record of no workgroups, which draws nothing
    auto const args = ctx->persistent.create_buffer_from_data(
        cc::vector<sg::dispatch_args>{{.x = 2}, {.x = 1}, {.x = 0}}, sg::buffer_usage::indirect_command_buffer);
    auto target = mesh_target();
    target.target_set = sg::test::sgl_shaders::meshes::painted::name;
    for (auto const [first, count, halves] : {cc::tuple{0, 1, 2}, cc::tuple{1, 2, 1}})
    {
        auto const pixels = co_await sg_test::draw_offscreen(*ctx, target,
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 scope.draw_mesh_tasks_indirect(args, first, count);
                                                             });
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                auto const is_left = x < size / 2;
                auto const expected = is_left     ? tg::vec4i(0, 0, 255, 255)
                                    : halves == 2 ? tg::vec4i(255, 0, 255, 255)
                                                  : tg::vec4i(0, 0, 0, 0);
                CHECK(pixels[0].rgba8(x, y) == expected).context(cc::format("{} halves, pixel ({}, {})", halves, x, y));
            }
    }
}
