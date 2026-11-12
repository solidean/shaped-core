#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// tests/shaders/sgl/meshes.sgl's mesh stages drawn as their polyfill, a compute pass and a draw, where the backend draws
// one (libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md), against the images the native stage draws in sgl-mesh-test.cc.
// Every probe is a pixel centre, and no edge of any triangle passes through one.

namespace
{
constexpr int size = 8;

sg_test::offscreen mesh_target()
{
    return {.width = size,
            .height = size,
            .colors = {sg::pixel_format::rgba8_unorm},
            .target_set = sg::test::sgl_shaders::meshes::painted::name};
}

/// Polyfills every mesh stage acquired while it lives, and restores what the context did before.
struct polyfilling
{
    sg::context& ctx;
    sg::polyfill_use before;

    explicit polyfilling(sg::context& c) : ctx(c), before(c.mesh_polyfill())
    {
        ctx.set_mesh_polyfill(sg::polyfill_use::always);
    }
    ~polyfilling() { ctx.set_mesh_polyfill(before); }
    polyfilling(polyfilling const&) = delete;
    polyfilling& operator=(polyfilling const&) = delete;
};

/// Why `ctx` cannot run these, or nothing where it can.
cc::optional<cc::string_view> unreachable(sg::context const& ctx)
{
    if (!sg_test::shaders_reach(ctx))
        return cc::string_view("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx.draws_mesh_polyfill())
        return cc::string_view("this backend draws no polyfilled mesh stage yet");
    return {};
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a polyfilled SGL mesh stage draws its triangle, and so its pipeline needs no mesh stage",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::covered);
    CHECK(pipeline->is_mesh_shaded());
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    CHECK(pipeline->mesh_polyfill()->corners == 3);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks(1);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(0, 255, 0, 255)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - two polyfilled SGL mesh workgroups each write their half, in slots of their own",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::split);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks(2);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const red = x < size / 2 ? 0 : 255;
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(red, 0, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - a polyfilled mesh stage keeps the native primitive order, and an unused slot draws nothing",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    // A draw of the split pipeline first leaves its slots in the scratch buffer, which the layered draw's unused slot
    // must not read as a triangle.
    auto const split = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::split);
    auto const layered = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::layered);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*split);
                                                             s.draw_mesh_tasks(2);
                                                             s.bind_pipeline(*layered);
                                                             s.draw_mesh_tasks(2);
                                                         });
    // workgroup 1's second triangle is drawn last: workgroup by workgroup, then by index
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(255, 255, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - an SGL mesh stage draws its triangles in order, workgroup by workgroup (CHK-480)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::mesh_shader))
        SKIP("this device has no mesh stage");

    // the native stage's own answer, which the polyfill is held to above
    auto const layered = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::layered);
    REQUIRE(layered->mesh_polyfill() == nullptr);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*layered);
                                                             s.draw_mesh_tasks(2);
                                                         });
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(255, 255, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
}

ASYNC_INVOCABLE_TEST("sg - a polyfilled task stage launches its mesh workgroups, each handed the payload",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    // `launching` states no bound, so the draw holds the budget's slots
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::launched);
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    CHECK(pipeline->mesh_polyfill()->task != nullptr);
    CHECK(pipeline->mesh_polyfill()->task_meshes == 0);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks(1);
                                                         });
    // sgl-mesh-test.cc's image of the native stages: red by the mesh workgroup's index, blue from the payload
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const red = x < size / 2 ? 0 : 255;
            CHECK(pixels[0].rgba8(x, y) == tg::vec4i(red, 0, 255, 255)).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - a polyfilled task workgroup that launches nothing draws nothing, and the rest keep their "
                     "order",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::culled);
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    CHECK(pipeline->mesh_polyfill()->task_meshes == 2);
    // twice, so the second draw's records follow the first's in a buffer the first left written
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks(4);
                                                             s.draw_mesh_tasks(4);
                                                         });
    // sgl-mesh-test.cc's image of the native stages: columns 2, 4 and 5 alone
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const expected = x == 2 ? tg::vec4i(64, 0, 255, 255)
                                : x == 4 ? tg::vec4i(128, 0, 255, 255)
                                : x == 5 ? tg::vec4i(128, 255, 255, 255)
                                         : tg::vec4i(0, 0, 0, 0);
            CHECK(pixels[0].rgba8(x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - a polyfilled mesh stage hands its pixel stage what it wrote per triangle, flat on each "
                     "corner",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::tinted);
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks(1);
                                                         });
    // sgl-mesh-test.cc's image of the native stage: triangle 0 red below the diagonal, triangle 1 blue above it
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            if (x + y == size - 1)
                continue;
            auto const expected = x + y > size - 1 ? tg::vec4i(255, 0, 0, 255) : tg::vec4i(0, 0, 255, 255);
            CHECK(pixels[0].rgba8(x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - an indirect draw of a polyfilled mesh stage reads its workgroup counts from a buffer, a "
                     "record at a time",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::split);
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    CHECK(pipeline->mesh_polyfill()->launched != nullptr);
    // sgl-mesh-test.cc's records and images of the native stage: both halves; then the left alone, beside a record of
    // no workgroups, which draws nothing
    auto const args = ctx->persistent.create_buffer_from_data(
        cc::vector<sg::dispatch_args>{{.x = 2}, {.x = 1}, {.x = 0}}, sg::buffer_usage::indirect_command_buffer);
    for (auto const [first, count, halves] : {cc::tuple{0, 1, 2}, cc::tuple{1, 2, 1}})
    {
        auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                             [&](sg::rendering_scope& s)
                                                             {
                                                                 s.bind_pipeline(*pipeline);
                                                                 s.draw_mesh_tasks_indirect(args, first, count);
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

ASYNC_INVOCABLE_TEST("sg - an indirect draw of a polyfilled task stage launches what its task workgroups do",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (auto const why = unreachable(*ctx); why.has_value())
        SKIP(why.value());
    auto const scope = polyfilling(*ctx);

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::culled);
    REQUIRE(pipeline->mesh_polyfill() != nullptr);
    CHECK(pipeline->mesh_polyfill()->launched == nullptr);
    // four task workgroups, then a record of none
    auto const args = ctx->persistent.create_buffer_from_data(cc::vector<sg::dispatch_args>{{.x = 4}, {.x = 0}},
                                                              sg::buffer_usage::indirect_command_buffer);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx, mesh_target(),
                                                         [&](sg::rendering_scope& s)
                                                         {
                                                             s.bind_pipeline(*pipeline);
                                                             s.draw_mesh_tasks_indirect(args, 0, 2);
                                                         });
    // the direct draw's image above: columns 2, 4 and 5 alone
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const expected = x == 2 ? tg::vec4i(64, 0, 255, 255)
                                : x == 4 ? tg::vec4i(128, 0, 255, 255)
                                : x == 5 ? tg::vec4i(128, 255, 255, 255)
                                         : tg::vec4i(0, 0, 0, 0);
            CHECK(pixels[0].rgba8(x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }
}
