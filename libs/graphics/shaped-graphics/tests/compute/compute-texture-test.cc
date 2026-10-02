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
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/textures.sgl, end to end: a texture sampled through a static and through a
// dynamic sampler, an image written, and an image read and written in one dispatch.

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

    auto const pipeline = co_await shaders::textures.copy_accumulate.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::post>();

    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::image);
    auto const acc = make_texture(ctx, sg::pixel_format::r32_float, sg::texture_usage::image);

    auto const texels = pattern();
    auto ones = cc::vector<float>::create_filled(k_extent * k_extent, 1.0f);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    cmd->upload.bytes_to_texture(acc.raw(), cc::as_bytes(ones));
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::post{
                                                               .texel_size = tg::vec2f(1.0f / k_extent, 1.0f / k_extent),
                                                               .src = src.as_texture_view(),
                                                               .dst = dst.as_image_view<sg::pixel_format::rgba8_unorm>(),
                                                               .acc = acc.as_image_view<sg::pixel_format::r32_float>(),
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

// dx12 places a group's static sampler in the register space of the slot it binds at, which differs per pipeline layout.
ASYNC_INVOCABLE_TEST("sg - one group's static sampler samples at whichever slot the entry point lists the group",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const at_slot_0 = co_await shaders::textures.copy_accumulate.acquire_pipeline(*ctx);
    auto const at_slot_1 = co_await shaders::textures.copy_second_slot.acquire_pipeline(*ctx);
    auto const post_layout = ctx->cached.acquire_binding_group_layout<shaders::post>();
    auto const sampled_layout = ctx->cached.acquire_binding_group_layout<shaders::sampled>();

    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::image);
    auto const acc = make_texture(ctx, sg::pixel_format::r32_float, sg::texture_usage::image);
    auto const second = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::image);
    auto const texels = pattern();

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    auto const post = ctx->transient.create_binding_group(*cmd, post_layout,
                                                          shaders::post{
                                                              .texel_size = tg::vec2f(1.0f / k_extent, 1.0f / k_extent),
                                                              .src = src.as_texture_view(),
                                                              .dst = dst.as_image_view<sg::pixel_format::rgba8_unorm>(),
                                                              .acc = acc.as_image_view<sg::pixel_format::r32_float>(),
                                                          });
    auto const sampled
        = ctx->transient.create_binding_group(*cmd, sampled_layout,
                                              shaders::sampled{
                                                  .src = src.as_texture_view(),
                                                  .smp = {},
                                                  .dst = second.as_image_view<sg::pixel_format::rgba8_unorm>(),
                                              });
    cmd->compute.bind_pipeline(*at_slot_0);
    cmd->compute.bind_group(0, *post);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    cmd->compute.bind_pipeline(*at_slot_1);
    cmd->compute.bind_group(0, *sampled);
    cmd->compute.bind_group(1, *post);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    auto const first_written = cmd->download.bytes_from_texture(dst.raw());
    auto const second_written = cmd->download.bytes_from_texture(second.raw());
    ctx->submit_command_list(cc::move(cmd));

    // A size mismatch counts every texel, so it fails the same check.
    auto const mismatches = [&](auto const& copied)
    {
        if (copied.size() != texels.size())
            return int(texels.size());
        auto count = 0;
        for (auto i = isize(0); i < texels.size(); ++i)
            count += copied[i] != texels[i] ? 1 : 0;
        return count;
    };
    auto const at_slot_0_copied = co_await first_written.bytes();
    CHECK(mismatches(at_slot_0_copied) == 0);
    auto const at_slot_1_copied = co_await second_written.bytes();
    CHECK(mismatches(at_slot_1_copied) == 0);
}

// The shader divides by the texture's size and bounds its store by the image's, so a wrong size shows in the texels.
ASYNC_INVOCABLE_TEST("sg - an SGL shader samples through a sampler the group binds", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::textures.copy_dynamic.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sampled>();

    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::image);
    auto const texels = pattern();

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    auto const group
        = ctx->transient.create_binding_group(*cmd, layout,
                                              shaders::sampled{
                                                  .src = src.as_texture_view(),
                                                  .smp = {.min_filter = sg::sampler_filter::nearest,
                                                          .mag_filter = sg::sampler_filter::nearest,
                                                          .mip_filter = sg::sampler_filter::nearest,
                                                          .address_u = sg::sampler_address_mode::clamp_edge,
                                                          .address_v = sg::sampler_address_mode::clamp_edge},
                                                  .dst = dst.as_image_view<sg::pixel_format::rgba8_unorm>(),
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

    // The texture this draw samples was uploaded in this list, so its barrier is found only at the draw, and a backend
    // that cannot hold it inside a pass splits the scope for it.
    // Nothing states a texture's first use before a scope yet; libs/graphics/shaped-graphics/docs/TODO.md, "Barriers + access tracking".
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    auto const& vs = co_await shaders::textures.screen_vs->acquire(*ctx);
    auto const& ps = co_await shaders::textures.textured_ps->acquire(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = shaders::textures.screen_vs.acquire_layout(*ctx),
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
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
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
        = ctx->transient.create_binding_group(*cmd, layout, shaders::material{.albedo = albedo.as_texture_view()});
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

namespace
{
/// How many bytes `pipeline`, a build of `textures.copy_clamped`, gets wrong: texel (x, y) samples texel (x - 8, y - 8)
/// at its centre, and the first half of each axis clamps to texel 0.
cc::shared_async<int> clamped_copy_mismatches(sg::context_handle ctx, sg::compute_pipeline_handle pipeline)
{
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::sampled>();
    auto const src = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::texture);
    auto const dst = make_texture(ctx, sg::pixel_format::rgba8_unorm, sg::texture_usage::image);
    auto const texels = pattern();

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), cc::span<byte const>(texels));
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::sampled{
                                                               .src = src.as_texture_view(),
                                                               .smp = {},
                                                               .dst = dst.as_image_view<sg::pixel_format::rgba8_unorm>(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    auto const written = cmd->download.bytes_from_texture(dst.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const copied = co_await written.bytes();
    CC_ASSERT(copied.size() == texels.size(), "the copy reads back whole");
    auto const half = k_extent / 2;
    auto mismatches = 0;
    for (auto y = 0; y < k_extent; ++y)
        for (auto x = 0; x < k_extent; ++x)
        {
            auto const from = (cc::max(y - half, 0) * k_extent + cc::max(x - half, 0)) * 4;
            for (auto c = 0; c < 4; ++c)
                mismatches += copied[(y * k_extent + x) * 4 + c] != texels[from + c] ? 1 : 0;
        }
    co_return mismatches;
}
} // namespace

// A file-scope sampler is the pipeline layout's rather than a group's: it clamps, and sg's default sampler repeats.
ASYNC_INVOCABLE_TEST("sg - an SGL shader samples through a sampler of the file, which its pipeline layout holds",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::textures.copy_clamped.acquire_pipeline(*ctx);
    CHECK((co_await clamped_copy_mismatches(ctx, pipeline)) == 0);
}

ASYNC_INVOCABLE_TEST("sg - a pipeline over static samplers keeps its own sampler when a cached blob seeds it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // One shader under two layouts that differ only in their bound sampler's address mode.
    // On dx12 such a pipeline neither hands out nor takes a blob, for docs/bugs-external/d3d12-cached-pso-static-sampler-mixup/.
    auto const& shader = co_await shaders::textures.copy_clamped->acquire(*ctx);
    auto const layout_with = [&](sg::sampler_address_mode address)
    {
        sg::bound_sampler const samplers[] = {{.binding = {.name = "clamped",
                                                           .space = slib::bound_samplers_space,
                                                           .index = 0,
                                                           .count = 1,
                                                           .type = sg::binding_type::sampler},
                                               .sampler = {.min_filter = sg::sampler_filter::nearest,
                                                           .mag_filter = sg::sampler_filter::nearest,
                                                           .address_u = address,
                                                           .address_v = address}}};
        return ctx->cached.acquire_pipeline_layout<shaders::sampled>(samplers);
    };
    auto const clamping = layout_with(sg::sampler_address_mode::clamp_edge);
    auto const first = co_await ctx->uncached.create_compute_pipeline_async({.shader = shader, .layout = clamping});
    auto const twin = co_await ctx->uncached.create_compute_pipeline_async(
        {.shader = shader, .layout = layout_with(sg::sampler_address_mode::repeat)});
    auto const rebuilt = co_await ctx->uncached.create_compute_pipeline_async(
        {.shader = shader, .layout = clamping, .cached_pipeline = first->cached_pipeline_data()});

    if (ctx->backend() == sg::backend_kind::dx12)
    {
        CHECK(first->cached_pipeline_data().empty());
        CHECK(!rebuilt->used_cached_pipeline());
    }
    CHECK((co_await clamped_copy_mismatches(ctx, twin)) != 0); // the twin repeats, so the check can tell them apart
    CHECK((co_await clamped_copy_mismatches(ctx, rebuilt)) == 0);
}

// The same sampler in a `pipeline` whose vertex stage does not reach it: the one layout of both stages holds it.
ASYNC_INVOCABLE_TEST("sg - an SGL pipeline carries the sampler of the file its pixel stage samples through",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // The texture this draw samples was uploaded in this list, so its barrier is found only at the draw, and a backend
    // that cannot hold it inside a pass splits the scope for it.
    // Nothing states a texture's first use before a scope yet; libs/graphics/shaped-graphics/docs/TODO.md, "Barriers + access tracking".
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::textures.clamped_draw);

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
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
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
        = ctx->transient.create_binding_group(*cmd, layout, shaders::material{.albedo = albedo.as_texture_view()});
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

    // Pixel x samples at 2u - 7/16: clamped, texel columns 0, 1, 3 and 3; repeated, they would be 3, 1, 3 and 1.
    int const column_of[extent] = {0, 1, 3, 3};
    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == texels.size());
    auto mismatches = 0;
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
            for (auto c = 0; c < 4; ++c)
                mismatches += pixels[(y * extent + x) * 4 + c] != texels[(y * extent + column_of[x]) * 4 + c] ? 1 : 0;
    CHECK(mismatches == 0);
}

namespace
{
/// Fills an image of `Format` through option_formats.sgl compiled for that format, and hands back its bytes.
/// `ctx` is by value, since a reference would dangle across a suspend.
template <sg::pixel_format Format>
cc::shared_async<cc::vector<byte>> fill_image(sg::context_handle ctx)
{
    auto const values = shaders::option_formats_fill_t::options{.output_format = Format};
    auto const pipeline = co_await shaders::option_formats.fill.acquire_pipeline(*ctx, values);
    // a helper is an unhomed async: it moves to where the device lives before its first bound call
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    // the group's layout follows the format its option has, as the shader's does
    auto const layout = ctx->cached.acquire_binding_group_layout(
        shaders::paint::declared_bindings({.output_format = Format}), shaders::paint::declared_samplers());
    auto const image = make_texture(ctx, Format, sg::texture_usage::image);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::paint{.color = tg::vec4f(0.0f, 1.0f, 0.25f, 1.0f), .target = image.as_image_view<Format>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_extent, k_extent);
    auto const written = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));
    auto const& bytes = co_await written.bytes();
    co_return cc::vector<byte>::create_copy_of(bytes);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an image whose format is an option is written in whichever format the host picks",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // 0, 1 and 0.25 as unorm bytes: 0.25 * 255 is 63.75, which rounds to 64
    auto const unorm = co_await fill_image<sg::pixel_format::rgba8_unorm>(ctx);
    REQUIRE(unorm.size() == isize(k_extent * k_extent * 4));
    auto wrong = 0;
    for (auto i = 0; i < k_extent * k_extent; ++i)
        wrong += unorm[i * 4] != byte(0) || unorm[i * 4 + 1] != byte(255) || unorm[i * 4 + 2] != byte(64)
                      || unorm[i * 4 + 3] != byte(255)
                   ? 1
                   : 0;
    CHECK(wrong == 0);

    // the same values as halves, each exact: 0x0000, 0x3c00, 0x3400 and 0x3c00
    auto const halves = co_await fill_image<sg::pixel_format::rgba16_float>(ctx);
    REQUIRE(halves.size() == isize(k_extent * k_extent * 8));
    constexpr u16 expected[] = {0x0000, 0x3c00, 0x3400, 0x3c00};
    wrong = 0;
    for (auto i = 0; i < k_extent * k_extent * 4; ++i)
    {
        auto bits = u16(0);
        cc::memcpy(&bits, halves.data() + i * 2, 2);
        wrong += bits != expected[i % 4] ? 1 : 0;
    }
    CHECK(wrong == 0);
}
