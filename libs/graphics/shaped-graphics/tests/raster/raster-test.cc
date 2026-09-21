#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/shader_asset.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// Tier 1's raster execution tests: a shader written once, drawn on every backend, and read back.

ASYNC_INVOCABLE_TEST("sg - a raster pipeline draws instanced quads from two vertex streams",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const& vs = co_await shaders::quads.vertex.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.pixel.main_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.vertex.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = shaders::target::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::target::name,
    });

    // One quad over the left half of clip space, drawn twice: once where it is, once moved onto the right half.
    auto const corners = ctx->persistent.create_buffer<shaders::quad::per_vertex>(
        6, sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const instances = ctx->persistent.create_buffer<shaders::quad::per_instance>(
        2, sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    auto const corner = [](float x, float y) { return shaders::quad::per_vertex{.corner = tg::vec3f(x, y, 0.0f)}; };
    shaders::quad::per_vertex const quad[] = {
        corner(-1, -1), corner(0, -1), corner(0, 1), corner(-1, -1), corner(0, 1), corner(-1, 1),
    };
    shaders::quad::per_instance const placed[] = {
        {.offset = tg::vec3f(0, 0, 0), .tint = tg::vec4f(1, 0, 0, 1)},
        {.offset = tg::vec3f(1, 0, 0), .tint = tg::vec4f(0, 0, 1, 1)},
    };

    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    cmd->upload.data_to_buffer(corners, quad);
    cmd->upload.data_to_buffer(instances, placed);
    {
        auto pass = cmd->raster.render_to(
            shaders::target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        pass.bind_pipeline(*pipeline);
        pass.bind_vertex_buffers(shaders::quad::buffers{.per_vertex = corners, .per_instance = instances}.views());
        pass.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = 0, .size = 2}});
    }
    auto const future = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == 4 * 4 * 4);
    auto const channel = [&](int x, int y, int c) { return int(pixels[(y * 4 + x) * 4 + c]); };
    for (auto y = 0; y < 4; ++y)
    {
        CHECK(channel(0, y, 0) == 255); // the first instance's red
        CHECK(channel(0, y, 2) == 0);
        CHECK(channel(3, y, 0) == 0); // the second's blue, moved by its own offset
        CHECK(channel(3, y, 2) == 255);
    }
}

ASYNC_INVOCABLE_TEST("sg - a pipeline built for one target set refuses a rendering of another, even where the formats "
                     "agree",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const& vs = co_await shaders::quads.vertex.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.pixel.overlay_ps->acquire(*ctx);
    // `overlay` has the shape of `target`, one float4, and is still another set.
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.vertex.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .color_targets = shaders::overlay::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::overlay::name,
    });
    CHECK(pipeline->target_set() == "sg::test::sgl_shaders::overlay");

    auto const image = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 4, .height = 4, .usage = sg::texture_usage::render_target});

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to(shaders::target{.color = image.as_render_target_view().discarded()});
        CHECK_ASSERTS(pass.bind_pipeline(*pipeline));
    }
    {
        auto pass = cmd->raster.render_to(shaders::overlay{.color = image.as_render_target_view().discarded()});
        pass.bind_pipeline(*pipeline);
    }
    ctx->submit_command_list(cc::move(cmd));
}
