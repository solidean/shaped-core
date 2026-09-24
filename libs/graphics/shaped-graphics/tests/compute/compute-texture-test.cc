#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/textures.sgl, end to end: a texture sampled through a static and through a
// bound sampler, an image written, and an image read and written in one dispatch.

namespace
{
constexpr int k_extent = 16;

sg::texture_2d make_texture(sg::context_handle const& ctx, sg::pixel_format format, sg::texture_usages usage)
{
    return sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = format,
        .dimension = sg::texture_dimension::d2,
        .width = k_extent,
        .height = k_extent,
        .usage = usage | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    }));
}

/// Four distinct channels per texel, so a swapped channel or a neighbour's texel shows.
cc::vector<byte> pattern()
{
    auto out = cc::vector<byte>();
    for (auto i = 0; i < k_extent * k_extent * 4; ++i)
        out.push_back(byte((i * 37 + 11) & 0xFF));
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL shader samples a texture through a static sampler and writes two images",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::textures.compute.copy_accumulate.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::post>();

    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::readonly_texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::readwrite_texture);
    auto const acc = make_texture(ctx, sg::pixel_format::r32_float, sg::texture_usage::readwrite_texture);

    auto const texels = pattern();
    auto ones = cc::vector<float>::create_filled(k_extent * k_extent, 1.0f);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    cmd->upload.bytes_to_texture(acc.raw(), cc::as_bytes(ones));
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::post{
                                                               .texel_size = tg::vec2f(1.0f / k_extent, 1.0f / k_extent),
                                                               .src = src.as_readonly_view(),
                                                               .dst = dst.as_readwrite_view(),
                                                               .acc = acc.as_readwrite_view(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    auto const written = cmd->download.bytes_from_texture(dst.raw());
    auto const accumulated = cmd->download.bytes_from_texture(acc.raw());
    ctx->submit_command_list(cc::move(cmd));

    // Every texel comes back unchanged: a linear sampler at a texel's centre filters nothing in.
    auto const copied = co_await written.bytes();
    REQUIRE(copied.size() == texels.size());
    auto mismatches = 0;
    for (auto i = isize(0); i < texels.size(); ++i)
        mismatches += copied[i] != texels[i] ? 1 : 0;
    CHECK(mismatches == 0);

    // The red channel of each texel, added onto the one the image held.
    auto const sums = co_await accumulated.bytes();
    REQUIRE(sums.size() == isize(k_extent * k_extent * 4));
    auto wrong = 0;
    for (auto i = 0; i < k_extent * k_extent; ++i)
    {
        auto value = 0.0f;
        cc::memcpy(&value, sums.data() + i * 4, 4);
        auto const expected = 1.0f + float(texels[i * 4]) / 255.0f;
        wrong += value > expected + 1e-5f || value < expected - 1e-5f ? 1 : 0;
    }
    CHECK(wrong == 0);
}

// The shader divides by the texture's size and bounds its store by the image's, so a wrong size shows in the texels.
ASYNC_INVOCABLE_TEST("sg - an SGL shader samples through a sampler the group binds", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::textures.compute.copy_dynamic.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sampled>();

    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::readonly_texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::readwrite_texture);
    auto const texels = pattern();

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout,
                                              shaders::sampled{
                                                  .src = src.as_readonly_view(),
                                                  .smp = {.min_filter = sg::sampler_filter::nearest,
                                                          .mag_filter = sg::sampler_filter::nearest,
                                                          .mip_filter = sg::sampler_filter::nearest,
                                                          .address_u = sg::sampler_address_mode::clamp_edge,
                                                          .address_v = sg::sampler_address_mode::clamp_edge},
                                                  .dst = dst.as_readwrite_view(),
                                              });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    auto const written = cmd->download.bytes_from_texture(dst.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const copied = co_await written.bytes();
    REQUIRE(copied.size() == texels.size());
    auto mismatches = 0;
    for (auto i = isize(0); i < texels.size(); ++i)
        mismatches += copied[i] != texels[i] ? 1 : 0;
    CHECK(mismatches == 0);
}

ASYNC_INVOCABLE_TEST("sg - an SGL pixel shader samples a texture at the level its derivatives pick",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const& vs = co_await shaders::textures.vertex.screen_vs->acquire(*ctx);
    auto const& ps = co_await shaders::textures.pixel.textured_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::textures.vertex.screen_vs.acquire_layout(*ctx),
        .vertex_shader = vs,
        .fragment_shader = ps,
        .vertex_input = shaders::screen_vertex::layout(),
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = shaders::textured_target::states{.color = {.format = sg::pixel_format::rgba8_unorm}},
        .target_set = shaders::textured_target::name,
    });

    // A 4 by 4 texture drawn onto a 4 by 4 target: each pixel's centre is a texel's, so the level is 0 and nothing filters in.
    // Every row is alike, so which way up a backend draws does not matter.
    constexpr int extent = 4;
    auto texels = cc::vector<byte>();
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
            texels.push_back_range(cc::span<byte const>({byte(40 + 60 * x), byte(200 - 50 * x), byte(10 * x), byte(255)}));
    auto const albedo = sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = extent,
        .height = extent,
        .usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst,
    }));
    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = extent,
                                             .height = extent,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto const corner = [](float x, float y, float u, float v)
    { return shaders::screen_vertex{.corner = tg::vec3f(x, y, 0.0f), .uv = tg::vec2f(u, v)}; };
    shaders::screen_vertex const quad[] = {
        corner(-1, -1, 0, 1), corner(1, -1, 1, 1), corner(1, 1, 1, 0),
        corner(-1, -1, 0, 1), corner(1, 1, 1, 0),  corner(-1, 1, 0, 0),
    };
    auto const vertices = ctx->persistent.create_buffer_from_data(quad, sg::buffer_usage::vertex_buffer);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(albedo.raw(), cc::span<byte const>(texels));
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::material>();
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout, shaders::material{.albedo = albedo.as_readonly_view()});
    {
        auto pass = cmd->raster.render_to(
            shaders::textured_target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        pass.bind_pipeline(*pipeline);
        pass.bind_group(0, *group);
        pass.bind_vertex_buffer(vertices.as_vertex_buffer());
        pass.draw({.vertex_range = {.offset = 0, .size = 6}});
    }
    auto const future = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == texels.size());
    auto mismatches = 0;
    for (auto i = isize(0); i < texels.size(); ++i)
        mismatches += pixels[i] != texels[i] ? 1 : 0;
    CHECK(mismatches == 0);
}
