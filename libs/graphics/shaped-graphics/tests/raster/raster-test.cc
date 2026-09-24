#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/fixed_vector.hh>
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

namespace
{
/// Why building `desc` failed, or empty when it built.
cc::shared_async<cc::string> refusal_of(sg::context& ctx, sg::raster_pipeline_description desc)
{
    auto const built = ctx.uncached.create_raster_pipeline_async(desc);
    co_await cc::async_settled(built);
    co_return built->has_error() ? built->try_error()->underlying().to_string() : cc::string();
}
} // namespace

// Tier 1's raster execution tests: a shader written once, drawn on every backend, and read back.

ASYNC_INVOCABLE_TEST("sg - a raster pipeline draws instanced quads from two vertex streams",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& vs = co_await shaders::quads.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.main_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = shaders::target::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::target::name,
    });

    // One quad over the left half of clip space, drawn twice: once where it is, once moved onto the right half.
    auto const corner = [](float x, float y) { return shaders::quad::per_vertex{.corner = tg::vec3f(x, y, 0.0f)}; };
    shaders::quad::per_vertex const quad[] = {
        corner(-1, -1), corner(0, -1), corner(0, 1), corner(-1, -1), corner(0, 1), corner(-1, 1),
    };
    shaders::quad::per_instance const placed[] = {
        {.offset = tg::vec3f(0, 0, 0), .tint = tg::vec4f(1, 0, 0, 1)},
        {.offset = tg::vec3f(1, 0, 0), .tint = tg::vec4f(0, 0, 1, 1)},
    };
    auto const corners = ctx->persistent.create_buffer_from_data(quad, sg::buffer_usage::vertex_buffer);
    auto const instances = ctx->persistent.create_buffer_from_data(placed, sg::buffer_usage::vertex_buffer);

    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
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

ASYNC_INVOCABLE_TEST("sg - a pipeline declared in SGL draws what the hand-built one does",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // Everything the hand-built description above states is in quads.sgl's `pipeline drawn`.
    auto const pipeline = co_await shaders::quads.drawn.acquire(*ctx);
    auto const desc = co_await shaders::quads.drawn.description(*ctx);
    CHECK(desc.rasterization.cull == sg::cull_mode::none);
    REQUIRE(desc.color_targets.size() == 1);
    CHECK(desc.color_targets[0].format == sg::pixel_format::rgba8_unorm);
    CHECK(desc.target_set == shaders::target::name);

    // `pipeline hosted` leaves the format to this call, and states the rest itself.
    auto const hosted = co_await shaders::quads.hosted.description(*ctx, {.color = sg::pixel_format::rgba16_float});
    CHECK(hosted.color_targets[0].format == sg::pixel_format::rgba16_float);
    CHECK(hosted.rasterization.cull == sg::cull_mode::none);
    // `customize` runs last, over what the declaration and the host stated: here over the host's format.
    auto const customized = co_await shaders::quads.hosted.acquire(
        *ctx, {.color = sg::pixel_format::rgba8_unorm},
        [](sg::raster_pipeline_description& d) { d.color_targets[0].format = sg::pixel_format::rgba16_float; });
    REQUIRE(customized->target_formats().has_value());
    CHECK(customized->target_formats().value().color[0] == sg::pixel_format::rgba16_float);

    auto const corner = [](float x, float y) { return shaders::quad::per_vertex{.corner = tg::vec3f(x, y, 0.0f)}; };
    shaders::quad::per_vertex const quad[] = {
        corner(-1, -1), corner(0, -1), corner(0, 1), corner(-1, -1), corner(0, 1), corner(-1, 1),
    };
    shaders::quad::per_instance const placed[] = {{.offset = tg::vec3f(1, 0, 0), .tint = tg::vec4f(0, 1, 0, 1)}};
    auto const corners = ctx->persistent.create_buffer_from_data(quad, sg::buffer_usage::vertex_buffer);
    auto const instances = ctx->persistent.create_buffer_from_data(placed, sg::buffer_usage::vertex_buffer);
    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to(
            shaders::target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        pass.bind_pipeline(*pipeline);
        pass.bind_vertex_buffers(shaders::quad::buffers{.per_vertex = corners, .per_instance = instances}.views());
        pass.draw({.vertex_range = {.offset = 0, .size = 6}, .instance_range = {.offset = 0, .size = 1}});
    }
    auto const future = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == 4 * 4 * 4);
    for (auto y = 0; y < 4; ++y)
    {
        CHECK(int(pixels[(y * 4 + 0) * 4 + 1]) == 0);   // the left half stays cleared
        CHECK(int(pixels[(y * 4 + 3) * 4 + 1]) == 255); // the one quad, moved onto the right half, is green
    }
}

ASYNC_INVOCABLE_TEST("sg - a pipeline built for one target set refuses a rendering of another, even where the formats "
                     "agree",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& vs = co_await shaders::quads.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.overlay_ps->acquire(*ctx);
    // `overlay` has the shape of `target`, one float4, and is still another set.
    // The description names none, so it is the one the pixel shader writes.
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .color_targets = shaders::overlay::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
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

ASYNC_INVOCABLE_TEST("sg - a pipeline refuses a rendering whose target formats differ from the ones it was built for",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& vs = co_await shaders::quads.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.main_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .color_targets = shaders::target::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::target::name,
    });
    REQUIRE(pipeline->target_formats().has_value());
    CHECK(pipeline->target_formats().value().color[0] == sg::pixel_format::rgba8_unorm);

    auto const target_of = [&](sg::pixel_format format)
    {
        return ctx->persistent.create_texture_2d(
            {.format = format, .width = 4, .height = 4, .usage = sg::texture_usage::render_target});
    };
    auto const rgba8 = target_of(sg::pixel_format::rgba8_unorm);
    auto const rgba16 = target_of(sg::pixel_format::rgba16_float);
    auto const depth = ctx->persistent.create_texture_2d(
        {.format = sg::pixel_format::depth32_float, .width = 4, .height = 4, .usage = sg::texture_usage::depth_stencil});

    auto cmd = ctx->create_command_list();
    {
        // The same target set, so only the format tells them apart.
        auto pass = cmd->raster.render_to(shaders::target{.color = rgba16.as_render_target_view().discarded()});
        CHECK_ASSERTS(pass.bind_pipeline(*pipeline));
    }
    {
        // A depth target the pipeline was not built with.
        auto pass = cmd->raster.render_to(shaders::target{.color = rgba8.as_render_target_view().discarded(),
                                                          .depth_stencil = depth.as_depth_stencil_view().discarded()});
        CHECK_ASSERTS(pass.bind_pipeline(*pipeline));
    }
    {
        auto pass = cmd->raster.render_to(shaders::target{.color = rgba8.as_render_target_view().discarded()});
        pass.bind_pipeline(*pipeline);
    }
    ctx->submit_command_list(cc::move(cmd));
}

ASYNC_INVOCABLE_TEST("sg - an SGL pixel shader states its targets, and a pipeline that disagrees is refused at "
                     "creation",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& vs = co_await shaders::quads.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.main_ps->acquire(*ctx);
    REQUIRE(ps.color_output_count.has_value());
    CHECK(ps.color_output_count.value() == 1);
    CHECK(ps.target_set == shaders::target::name);

    auto const layout = shaders::quads.main_vs.acquire_layout(*ctx);
    auto const rgba = sg::color_target_state{.format = sg::pixel_format::rgba8_unorm};
    auto const described
        = [&](cc::fixed_vector<sg::color_target_state, sg::max_color_targets> targets, cc::string_view target_set)
    {
        return sg::raster_pipeline_description{.layout = layout,
                                               .vertex_shader = vs,
                                               .fragment_shader = ps,
                                               .vertex_input = shaders::quad::layout(),
                                               .color_targets = cc::move(targets),
                                               .target_set = target_set};
    };

    // A shader is input, so each is a refusal the caller receives, and nothing asserts.
    // One target written, two declared: the second is left undefined unless its write mask is empty.
    // (An empty one differs from the first target's, which vulkan without independentBlend refuses, so it is not
    // built here; the dx12 pipeline-cache test builds one.)
    CHECK((co_await refusal_of(*ctx, described({rgba, rgba}, ""))).contains("write mask must be empty"));
    // One written, none declared: the output goes nowhere.
    CHECK((co_await refusal_of(*ctx, described({}, ""))).contains("writes 1 color targets, and the pipeline has 0"));
    // A description that names another set than the shader writes.
    CHECK((co_await refusal_of(*ctx, described({rgba}, shaders::overlay::name))).contains("and the pipeline names"));
}

ASYNC_INVOCABLE_TEST("sg - a 32-bit indexed draw honours an odd first index", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // **The positive half of the index-fetch alignment rule**, which
    // libs/graphics/shaped-graphics/tests/command_list/index_buffer_alignment-test.cc only states in the negative.
    // 32-bit indices are the fix that rule names for a sub-mesh whose first index is odd, so an odd first index into
    // one must draw exactly what it names.
    // `sr::imgui_routine` is the caller that rests on it: an ImDrawCmd's first index is arbitrary, which is why its
    // draw indices are u32.
    auto const& vs = co_await shaders::quads.main_vs->acquire(*ctx);
    auto const& ps = co_await shaders::quads.main_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::quads.main_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::quad::layout(),
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = shaders::target::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::target::name,
    });

    auto const corner = [](float x, float y) { return shaders::quad::per_vertex{.corner = tg::vec3f(x, y, 0.0f)}; };
    // The left half's corners first, then the right half's, so the two halves are reachable by index alone.
    shaders::quad::per_vertex const corners[] = {
        corner(-1, -1), corner(0, -1), corner(0, 1), corner(-1, 1), corner(1, -1), corner(1, 1),
    };
    shaders::quad::per_instance const placed[] = {{.offset = tg::vec3f(0, 0, 0), .tint = tg::vec4f(1, 0, 0, 1)}};

    // Seven indices, and the draw starts at the second: index 0 is a decoy that only a draw ignoring the first index
    // would read.
    // Reading from 0 would draw the bottom-left triangle (0, 1, 4) instead, which is a different picture rather than
    // a shifted one — so the check below distinguishes them.
    u32 const indices[] = {0, 1, 4, 5, 1, 5, 2};

    auto const vertex_buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const instance_buffer = ctx->persistent.create_buffer_from_data(placed, sg::buffer_usage::vertex_buffer);
    auto const index_buffer = ctx->persistent.create_buffer_from_data(indices, sg::buffer_usage::index_buffer);

    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to(
            shaders::target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        pass.bind_pipeline(*pipeline);
        pass.bind_vertex_buffers(
            shaders::quad::buffers{.per_vertex = vertex_buffer, .per_instance = instance_buffer}.views());
        pass.bind_index_buffer(index_buffer.as_index_buffer());
        pass.draw_indexed({.index_range = {.offset = 1, .size = 6}});
    }
    auto const future = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == 4 * 4 * 4);
    auto const red_at = [&](int x, int y) { return int(pixels[(y * 4 + x) * 4]); };
    for (auto y = 0; y < 4; ++y)
    {
        CHECK(red_at(0, y) == 0); // the left half the decoy index would have reached into
        CHECK(red_at(1, y) == 0);
        CHECK(red_at(2, y) == 255); // the right-half quad indices 1..6 name
        CHECK(red_at(3, y) == 255);
    }
}
