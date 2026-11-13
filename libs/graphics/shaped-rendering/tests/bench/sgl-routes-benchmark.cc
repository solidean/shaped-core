#include "../shader_fixtures.hh"
#include "bench_support.hh"

#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#if SR_BENCH_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif
#include <shaped-shader-library/shader_asset.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// sr's SGL kernels that have no hand-written original, each through both of SGL's routes on one device.
// Its text through DXC is the contender every ratio is of, and SGL's own binary of the same source the other.
// sgl-against-hlsl-benchmark.cc times the kernels that do have an original, and its method is this file's.

namespace
{
using namespace sr_bench::impl;
using record_fn = cc::unique_function<void(sg::command_list&, sg::compute_pipeline const&)>;

/// A texture of `format` and `extent`, sampled and written as an image.
[[nodiscard]] sg::texture_2d image_texture(sg::context& ctx, sg::pixel_format format, tg::vec2i extent)
{
    return ctx.persistent.create_texture_2d(
        {.format = format,
         .width = extent[0],
         .height = extent[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst});
}

/// An rg32_float texture of `k_extent`, a pixel's luminance moments: a mean and a mean square of a noisy value.
cc::shared_async<sg::texture_2d> filled_moments(sg::context& ctx)
{
    auto texels = cc::vector<tg::vec2f>();
    texels.reserve(isize(k_extent[0]) * k_extent[1]);
    for (auto i = 0; i < k_extent[0] * k_extent[1]; ++i)
    {
        auto const l = 0.2f + noise(u32(i) * 31u + 17u);
        texels.push_back(tg::vec2f(l, l * l * 1.1f));
    }
    auto t = image_texture(ctx, sg::pixel_format::rg32_float, k_extent);
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(t.raw(), cc::span<tg::vec2f const>(texels).as_bytes());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_return t;
}

/// The text and the native pipeline of `entry`, under `layout` and `options`, timed against each other as `name`.
template <class Entry>
cc::shared_async<cc::unit> time_routes(sg::context& ctx,
                                       cc::string_view name,
                                       Entry const& entry,
                                       sg::pipeline_layout_handle layout,
                                       cc::vector<slib::shader_option> options,
                                       record_fn record)
{
    auto const text_pipeline = slib::acquire_compute_pipeline(&ctx, entry.asset, layout, options);
    auto const native_pipeline
        = slib::acquire_compute_pipeline(&ctx, entry.asset, layout, with_native(ctx, cc::move(options)));
    auto contenders = cc::vector<contender>();
    contenders.push_back(contender_of("text", ctx, text_pipeline, &record));
    contenders.push_back(contender_of("native", ctx, native_pipeline, &record));
    co_await measure(ctx, name, contenders);
}
} // namespace

/// Every kernel on `ctx`, its text through DXC against SGL's own binary.
static cc::shared_async<cc::unit> compare_routes_on(sg::context& ctx)
{
    CC_LOG_INFO("{} adapter: {}", ctx.backend() == sg::backend_kind::vulkan ? "vulkan" : "dx12",
                ctx.metrics.adapter().name);
    // the library sr's packages resolve their assets through
    (void)sr_test::shader_fixtures();
    ctx.upload.set_inline_budget(isize(64) << 20);
    ctx.advance_epoch();

    auto const color = co_await filled_texture(
        ctx, 1, [](i32, i32, u32 s) { return tg::vec4f(0.2f + noise(s), noise(s + 7), noise(s + 13), 1.0f); });
    auto const half = k_extent / 2;
    auto const quarter = k_extent / 4;

    // ---- the BC encoders, one 1024x1024 image each --------------------------------------------------------------------
    {
        namespace bc = sr::sgl_shaders::compression_bc;
        auto const blocks_wide = (k_extent[0] + 3) / 4;
        auto const blocks_high = (k_extent[1] + 3) / 4;
        auto const constants = bc::bc_constants{.blocks_wide = blocks_wide,
                                                .blocks_high = blocks_high,
                                                .row_blocks = blocks_wide,
                                                .width = k_extent[0],
                                                .height = k_extent[1],
                                                .srgb = 0,
                                                .bc1_cut_out = 0,
                                                .channel_range = 0,
                                                .snorm = 0};
        auto const usage = sg::buffer_usage::readwrite_buffer;
        auto const narrow = ctx.persistent.create_buffer<tg::vec<2, u32>>(blocks_wide * blocks_high, usage);
        auto const wide = ctx.persistent.create_buffer<tg::vec<4, u32>>(blocks_wide * blocks_high, usage);
        auto const narrow_layout = ctx.cached.acquire_binding_group_layout<bc::bc_io8>();
        auto const wide_layout = ctx.cached.acquire_binding_group_layout<bc::bc_io16>();
        // bc3, bc5 and bc7 write 16 bytes a block, bc1 and bc4 eight
        auto const encoder = [&](cc::string_view name, auto const& entry, bool is_wide) -> cc::shared_async<cc::unit>
        {
            co_await time_routes(
                ctx, name, entry, entry.acquire_layout(ctx), {},
                [&, is_wide](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
                {
                    auto const group
                        = is_wide
                            ? ctx.transient.create_binding_group(
                                  cmd, wide_layout,
                                  bc::bc_io16{.source = color.as_texture_view(), .blocks = wide.as_readwrite_buffer()})
                            : ctx.transient.create_binding_group(
                                  cmd, narrow_layout,
                                  bc::bc_io8{.source = color.as_texture_view(), .blocks = narrow.as_readwrite_buffer()});
                    cmd.compute.bind_pipeline(pipeline);
                    cmd.compute.bind_group(0, *group);
                    cmd.compute.set_inline_constants(constants.to_block());
                    cmd.compute.dispatch_threads(blocks_wide, blocks_high);
                });
        };
        co_await encoder("bc1", bc::bc1_cs, false);
        co_await encoder("bc3", bc::bc3_cs, true);
        co_await encoder("bc4", bc::bc4_cs, false);
        co_await encoder("bc5", bc::bc5_cs, true);
        co_await encoder("bc7", bc::bc7_cs, true);
    }

    // ---- bloom, each pass at the level the routine runs it at for a 1024x1024 image -----------------------------------
    {
        namespace bloom = sr::sgl_shaders::bloom;
        auto const level = image_texture(ctx, sg::pixel_format::rgba16_float, half);
        auto const coarser = image_texture(ctx, sg::pixel_format::rgba16_float, quarter);
        auto const carried = image_texture(ctx, sg::pixel_format::rgba16_float, half);
        auto const out = image_texture(ctx, sg::pixel_format::rgba16_float, k_extent);
        auto const first_layout = ctx.cached.acquire_binding_group_layout<bloom::bloom_first_io>();
        auto const chain_layout = ctx.cached.acquire_binding_group_layout<bloom::bloom_chain_io>();
        auto const finish_layout = ctx.cached.acquire_binding_group_layout<bloom::bloom_finish_io>();
        auto const constants = bloom::bloom_constants{.knee = 0.8f, .weight = 0.5f, .carry = 0.7f};

        // the first pass fills the level the others read, so they read light rather than zeros
        co_await time_routes(
            ctx, "bloom first", bloom::first_cs, bloom::first_cs.acquire_layout(ctx), {},
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, first_layout,
                    bloom::bloom_first_io{.source = color.as_texture_view(),
                                          .target = level.as_image_view<sg::pixel_format::rgba16_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(constants.to_block());
                cmd.compute.dispatch_threads(half[0], half[1], 1);
            });
        co_await time_routes(
            ctx, "bloom down", bloom::down_cs, bloom::down_cs.acquire_layout(ctx), {},
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, chain_layout,
                    bloom::bloom_chain_io{.source = level.as_texture_view(),
                                          .level = level.as_texture_view(),
                                          .target = coarser.as_image_view<sg::pixel_format::rgba16_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.dispatch_threads(quarter[0], quarter[1], 1);
            });
        co_await time_routes(
            ctx, "bloom up", bloom::up_cs, bloom::up_cs.acquire_layout(ctx), {},
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, chain_layout,
                    bloom::bloom_chain_io{.source = coarser.as_texture_view(),
                                          .level = level.as_texture_view(),
                                          .target = carried.as_image_view<sg::pixel_format::rgba16_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(constants.to_block());
                cmd.compute.dispatch_threads(half[0], half[1], 1);
            });
        co_await time_routes(
            ctx, "bloom finish", bloom::finish_cs, bloom::finish_cs.acquire_layout(ctx), {},
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, finish_layout,
                    bloom::bloom_finish_io{.source = color.as_texture_view(),
                                           .spread = carried.as_texture_view(),
                                           .target = out.as_image_view<sg::pixel_format::rgba16_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(constants.to_block());
                cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
            });
    }

    // ---- the box filter's 2D level, 1024x1024 into an rgba8 512x512 -----------------------------------------------------
    {
        namespace mip = sr::sgl_shaders::box_filter_mipmap;
        auto const values = mip::options{};
        auto const next = image_texture(ctx, sg::pixel_format::rgba8_unorm, half);
        auto const group_layout = ctx.cached.acquire_binding_group_layout(mip::mipmap_2d::declared_bindings(values));
        co_await time_routes(
            ctx, "mipmap 2d", mip::main_2d_cs, mip::main_2d_cs.acquire_layout(ctx, values), values.values(),
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, group_layout,
                    mip::mipmap_2d{.source = color.as_texture_view(), .target = next.as_any_image_view()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.dispatch_threads(half[0], half[1], 1);
            });
    }

    // ---- the exposure meter, its one group of 256 threads over a 1024x1024 image --------------------------------------
    {
        namespace meter = sr::sgl_shaders::exposure_meter;
        auto const partials = ctx.persistent.create_buffer<tg::vec2f>(256, sg::buffer_usage::readwrite_buffer);
        auto const group_layout = ctx.cached.acquire_binding_group_layout<meter::meter_io>();
        co_await time_routes(
            ctx, "exposure meter", meter::main_cs, meter::main_cs.acquire_layout(ctx), {},
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, group_layout,
                    meter::meter_io{.source = color.as_texture_view(), .partials = partials.as_readwrite_buffer()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.dispatch_threads(256, 1, 1);
            });
    }

    // ---- SVGF's temporal and variance passes, over a history young enough that every pixel estimates spatially ------
    {
        namespace svgf = sr::sgl_shaders::svgf;
        auto const values = svgf::options{.target_format = sg::pixel_format::rgba32_float};
        auto const albedo = co_await filled_texture(
            ctx, 2, [](i32, i32, u32 s) { return tg::vec4f(0.5f, 0.6f, 0.7f, 1.0f) * (0.5f + 0.5f * noise(s)); });
        auto const normal = co_await filled_texture(
            ctx, 3, [](i32 x, i32 y, u32)
            { return tg::vec4f(f32(x % 64) / 64.0f - 0.5f, f32(y % 64) / 64.0f - 0.5f, 1.0f, 0.0f); });
        auto const depth
            = co_await filled_texture(ctx, 4, [](i32 x, i32, u32 s)
                                      { return tg::vec4f(1.0f + f32(x / 128) + 0.01f * noise(s), 0.0f, 0.0f, 0.0f); });
        auto const normal_depth = co_await filled_texture(
            ctx, 5, [](i32 x, i32 y, u32) { return tg::vec4f(0.0f, 0.0f, 1.0f, 1.0f + f32((x + y) / 128)); });
        // a motion of a pixel or two, so some pixels look back outside the image and the rest at a neighbor
        auto const motion = co_await filled_texture(
            ctx, 6, [](i32, i32, u32 s) { return tg::vec4f(2.0f * noise(s) - 1.0f, 2.0f * noise(s + 3) - 1.0f, 0, 0); });
        // a history of two frames, below the variance pass's spatial threshold
        auto const history = co_await filled_texture(
            ctx, 7, [](i32, i32, u32 s) { return tg::vec4f(noise(s), noise(s + 5), noise(s + 9), 2.0f); });
        auto const moments = co_await filled_moments(ctx);
        auto const history_out = image_texture(ctx, sg::pixel_format::rgba32_float, k_extent);
        auto const moments_out = image_texture(ctx, sg::pixel_format::rg32_float, k_extent);
        auto const normal_depth_out = image_texture(ctx, sg::pixel_format::rgba32_float, k_extent);
        auto const target = target_texture(ctx);
        auto const temporal_layout = ctx.cached.acquire_binding_group_layout<svgf::svgf_temporal_images>();
        auto const variance_layout = ctx.cached.acquire_binding_group_layout<svgf::svgf_variance_images>();

        co_await time_routes(
            ctx, "svgf temporal", svgf::temporal_cs, svgf::temporal_cs.acquire_layout(ctx, values), values.values(),
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, temporal_layout,
                    svgf::svgf_temporal_images{
                        .color = color.as_texture_view(),
                        .albedo = albedo.as_texture_view(),
                        .specular_albedo = albedo.as_texture_view(),
                        .normal = normal.as_texture_view(),
                        .depth = depth.as_texture_view(),
                        .motion = motion.as_texture_view(),
                        .previous_history = history.as_texture_view(),
                        .previous_moments = moments.as_texture_view(),
                        .previous_normal_depth = normal_depth.as_texture_view(),
                        .history = history_out.as_image_view<sg::pixel_format::rgba32_float>(),
                        .moments = moments_out.as_image_view<sg::pixel_format::rg32_float>(),
                        .normal_depth = normal_depth_out.as_image_view<sg::pixel_format::rgba32_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(svgf::svgf_temporal_constants{.flags = 1u,
                                                                               .color_alpha_min = 0.2f,
                                                                               .moments_alpha_min = 0.2f,
                                                                               .max_history = 32.0f,
                                                                               .normal_similarity = 0.9f,
                                                                               .depth_similarity = 0.1f}
                                                     .to_block());
                cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
            });
        co_await time_routes(
            ctx, "svgf variance", svgf::variance_cs, svgf::variance_cs.acquire_layout(ctx, values), values.values(),
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, variance_layout,
                    svgf::svgf_variance_images{.history = history.as_texture_view(),
                                               .moments = moments.as_texture_view(),
                                               .normal_depth = normal_depth.as_texture_view(),
                                               .target = target.as_image_view<sg::pixel_format::rgba32_float>()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(
                    svgf::svgf_variance_constants{.normal_power = 128.0f, .depth_sigma = 1.0f, .spatial_below = 4.0f}
                        .to_block());
                cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
            });
    }

    co_await ctx.idle_completion();
}

// The kernels with no original of their own, under the name the run in
// libs/graphics/shaped-graphics-language/docs/native-parity.md selects.
ASYNC_BENCHMARK("sr - SGL's own binary against its text through DXC, the kernels with no HLSL original, on dx12")
{
    auto created = sg::create_dx12_context({.native_dxil = false, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    co_await compare_routes_on(*created.value());
}

#if SR_BENCH_HAS_VULKAN
ASYNC_BENCHMARK("sr - SGL's own binary against its text through DXC, the kernels with no HLSL original, on vulkan")
{
    // what the driver made of each route's pipeline, beside the time it takes
    auto created = sg::create_vulkan_context({.native_spirv = false, .capture_driver_statistics = true});
    if (created.has_error())
        SKIP("no vulkan device");
    co_await compare_routes_on(*created.value());
}
#endif
