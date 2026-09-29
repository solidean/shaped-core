#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

ASYNC_INVOCABLE_TEST("sg - every vertex_attribute_format decodes the value its bytes hold",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // vertex_input.sgl compares each member against these values and sets one bit per member that matched.
    // The two packed formats share their bytes, 51, 102, 153 and 255, which unorm reads as 0.2, 0.4, 0.6 and 1.0.
    auto const vertex = shaders::formatted{
        .f1 = 1.5f,
        .f2 = tg::vec2f(2.5f, -3.0f),
        .f3 = tg::vec3f(4.0f, 5.5f, -6.25f),
        .f4 = tg::vec4f(7.0f, 8.5f, 9.75f, -10.0f),
        .i1 = -7,
        .i2 = tg::vec2i(11, -12),
        .i3 = tg::vec3i(13, -14, 15),
        .i4 = tg::vec4i(-16, 17, -18, 19),
        .u1 = 4000000000u,
        .u2 = tg::vec<2, u32>(2147483650u, 23u),
        .u3 = tg::vec<3, u32>(24u, 3000000000u, 25u),
        .u4 = tg::vec<4, u32>(26u, 27u, 4294967295u, 28u),
        .n4 = 0xFF996633u,
        .b4 = 0xFF996633u,
    };
    shaders::formatted const vertices[] = {vertex};
    auto const buffer = ctx->persistent.create_buffer_from_data(vertices, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::vertex_input.formats);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 1,
                                                          .height = 1,
                                                          .colors = {sg::pixel_format::rgba32_float},
                                                          .target_set = shaders::format_target::name,
                                                          .clear_color = tg::vec4f(-1, 0, 0, 0)},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 1}});
                                                         });

    auto const matched = int(pixels[0].rgba_float(0, 0)[0]);
    constexpr char const* members[] = {
        "f1", "f2", "f3", "f4", "i1", "i2", "i3", "i4", "u1", "u2", "u3", "u4", "n4 (rgba8_unorm)", "b4 (rgba8_uint)"};
    REQUIRE(matched >= 0);
    for (auto i = 0; i < 14; ++i)
        CHECK((matched & (1 << i)) != 0).context(cc::format("vertex member {} decoded wrongly", members[i]));
}
