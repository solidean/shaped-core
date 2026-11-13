#include "../../pipeline/pipeline_harness.hh"
#include "../differential.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sg_test_differential_setups.hh>
#include <sgl_modules/diff_floats.hh>
#include <sgl_modules/diff_raster.hh>
#include <sgl_modules/diff_texels.hh>
#include <sgl_modules/diff_words.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/shader_asset.hh>

// The corpus's setups, each the C++ half of the SGL module of its name.
// Every input is generated rather than random, so every run on every machine reads the same ones.

using namespace cc::primitive_defines;

namespace
{
constexpr isize k_threads = 256;
constexpr isize k_outputs_per_thread = 4;
constexpr int k_extent = 16;
constexpr int k_levels = 5; // 16, 8, 4, 2 and 1

[[nodiscard]] u32 hashed(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb'352du;
    x ^= x >> 15;
    x *= 0x846c'a68bu;
    return x ^ (x >> 16);
}

[[nodiscard]] cc::vector<u32> word_inputs()
{
    auto words = cc::vector<u32>{0u, 1u, 0x8000'0000u, 0xffff'ffffu, 0x7fff'ffffu, 0x8000'0001u, 2u, 31u, 32u, 33u};
    for (auto i = words.size(); i < k_threads; ++i)
        words.push_back(hashed(u32(i)));
    return words;
}

/// -64 to 64, never zero, of every magnitude between: small fractions, whole numbers and values just off them.
[[nodiscard]] cc::vector<f32> float_inputs()
{
    auto floats = cc::vector<f32>();
    for (auto i = isize(0); i < k_threads; ++i)
    {
        auto const unit = f32(hashed(u32(i) + 0x9e37'79b9u) >> 8) / f32(1 << 24); // [0, 1)
        auto const magnitude = (i % 4 == 0) ? 64.0f * unit : (i % 4 == 1) ? unit : f32(i % 17) + 0.5f * unit;
        auto const value = cc::max(magnitude, 1.0f / 1024.0f);
        floats.push_back(i % 2 == 0 ? value : -value);
    }
    return floats;
}

[[nodiscard]] cc::vector<sg_diff::output> one_output(cc::string_view name, cc::span<byte const> bytes)
{
    auto result = cc::vector<sg_diff::output>();
    result.push_back({.name = cc::string(name), .bytes = cc::vector<byte>::create_copy_of(bytes)});
    return result;
}

/// The 16 by 16 rgba8_unorm texture of every texture-reading setup, each texel of each level its own hashed color.
/// Uploaded on a list of its own, so the list that reads it opens no barrier inside a scope.
[[nodiscard]] sg::texture_2d hashed_texture(sg::context& ctx)
{
    auto const texture
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = k_extent,
                                            .height = k_extent,
                                            .mip_levels = k_levels,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto cmd = ctx.create_command_list();
    for (auto level = 0; level < k_levels; ++level)
    {
        auto const side = k_extent >> level;
        auto texels = cc::vector<u32>();
        for (auto i = 0; i < side * side; ++i)
            texels.push_back(hashed(u32(level * 4096 + i)) | 0xff00'0000u);
        cmd->upload.bytes_to_texture(texture.raw(), cc::span<u32 const>(texels).as_bytes(), {.mip_level = level});
    }
    ctx.submit_command_list(cc::move(cmd));
    return texture;
}

template <class Group, class T>
cc::shared_async<cc::vector<sg_diff::output>> run_buffers(sg::context* ctx,
                                                          sg_diff::shaders_on_route on,
                                                          cc::vector<T> inputs)
{
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<Group>()});
    auto const input = ctx->persistent.create_buffer_from_data(inputs, sg::buffer_usage::readonly_buffer);
    auto const output
        = ctx->persistent.create_buffer_from_data(cc::vector<T>::create_filled(k_threads * k_outputs_per_thread, T(0)),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<Group>();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, Group{.input = input.as_readonly_buffer(), .output = output.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(output);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    co_return one_output("output", cc::span<T const>(data).as_bytes());
}

cc::shared_async<cc::vector<sg_diff::output>> run_texels(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_texels::io;
    auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
        {.shader = on.entry, .layout = ctx->cached.acquire_pipeline_layout<io>()});
    auto const source = hashed_texture(*ctx);
    auto const output = ctx->persistent.create_buffer_from_data(
        cc::vector<tg::vec4f>::create_defaulted(k_threads * k_outputs_per_thread),
        sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<io>();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, io{.source = source.as_texture_view(), .output = output.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(output);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    co_return one_output("output", cc::span<tg::vec4f const>(data).as_bytes());
}

cc::shared_async<cc::vector<sg_diff::output>> run_raster(sg::context* ctx, sg_diff::shaders_on_route on)
{
    using io = sgl_modules::diff_raster::io;
    using target = sgl_modules::diff_raster::target;
    auto const vertex
        = co_await sg_diff::compiled_on(*ctx, on.route, *sg::test::differential_setups::fullscreen::fullscreen.asset);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline({
        .layout = ctx->cached.acquire_pipeline_layout<io>(),
        .vertex_shader = vertex,
        .fragment_shader = on.entry,
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = target::states{.color = {.format = sg::pixel_format::rgba32_float}},
        .target_set = target::name,
    });
    auto const input = ctx->persistent.create_buffer_from_data(float_inputs(), sg::buffer_usage::readonly_buffer);
    auto const source = hashed_texture(*ctx);
    auto const group = ctx->persistent.create_binding_group(
        ctx->cached.acquire_binding_group_layout<io>(),
        io{.input = input.as_readonly_buffer(), .source = source.as_texture_view()});

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = k_extent, .height = k_extent, .colors = {sg::pixel_format::rgba32_float}, .target_set = target::name},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.bind_group(0, *group);
            scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        });
    co_return one_output("color", pixels.colors[0].bytes);
}

bool const words_registered = sg_diff::register_setup({
    .name = "words",
    .run = [](sg::context* ctx, sg_diff::shaders_on_route on)
    { return run_buffers<sgl_modules::diff_words::io>(ctx, cc::move(on), word_inputs()); },
    .routes = {},
    .across = {},
});

bool const floats_registered = sg_diff::register_setup({
    .name = "floats",
    .run = [](sg::context* ctx, sg_diff::shaders_on_route on)
    { return run_buffers<sgl_modules::diff_floats::io>(ctx, cc::move(on), float_inputs()); },
    .routes = {.kind = sg_diff::tolerance::measure::ulps, .amount = 2},
    .across = {.kind = sg_diff::tolerance::measure::ulps, .amount = 16},
});

// A filtered sample's weights carry as few as 8 fractional bits on some hardware, so across APIs a texel's difference is bounded by its 256th.
bool const texels_registered = sg_diff::register_setup({
    .name = "texels",
    .run = &run_texels,
    .routes = {},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
});

bool const raster_registered = sg_diff::register_setup({
    .name = "raster",
    .stage = sg::shader_stage::fragment,
    .run = &run_raster,
    .routes = {.kind = sg_diff::tolerance::measure::ulps, .amount = 2},
    .across = {.kind = sg_diff::tolerance::measure::relative, .amount = 0.005},
});
} // namespace
