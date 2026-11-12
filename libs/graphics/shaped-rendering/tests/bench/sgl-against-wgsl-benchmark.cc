#include "../shader_fixtures.hh"
#include "bench_support.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_bench_wgsl_shaders.hh>
#include <sr_sgl_shaders.hh>

// What sr ships in SGL against the same kernels written by hand in WGSL, on WebGPU.
//
// The WGSL under tests/bench/shaders ports the HLSL originals line for line, and declares the bindings SGL's port does,
// so one group layout and one binding group serve both contenders and only the shader differs.
// The method is sgl-against-hlsl-benchmark.cc's: the two of a pair alternate, and each pair logs medians and a ratio.

namespace
{
using namespace sr_bench::impl;

/// The pipeline of a hand-written WGSL shader under SGL's group layout and its own reflected constants.
/// Empty while the shader is still building, and stays empty for one that failed, which `is_ready` then never reports.
struct wgsl_pipeline
{
    sg::async_compiled_shader shader;
    sg::binding_group_layout_handle group_layout;
    sg::async_compute_pipeline pipeline;

    [[nodiscard]] bool is_ready(sg::context& ctx)
    {
        if (pipeline.is_valid())
        {
            (void)ctx.backlog.start(pipeline);
            if (pipeline->has_error())
                REQUIRE(false).dump("error", cc::format("{}", pipeline->try_error()->underlying()));
            return pipeline->is_ready() && pipeline->try_value() != nullptr;
        }
        (void)ctx.backlog.start(shader);
        if (shader->has_error())
            REQUIRE(false).dump("error", cc::format("{}", shader->try_error()->underlying()));
        if (!shader->is_ready() || shader->try_value() == nullptr)
            return false;
        auto const* const compiled = shader->try_value();
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::constants_buffer)
            {
                auto const layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}, .inline_constants = b});
                pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
            }
        return false;
    }
};

/// SGL's bindings as the WGSL names them: a WGSL identifier has no `.`, so `atrous_images.source` is
/// `atrous_images_source`, at the same slot and of the same type.
[[nodiscard]] cc::vector<sg::binding> wgsl_named(cc::span<sg::binding const> bindings)
{
    auto renamed = cc::vector<sg::binding>::create_copy_of(bindings);
    for (auto& b : renamed)
    {
        for (auto& c : b.name.as_mutable_span())
            if (c == '.')
                c = '_';
    }
    return renamed;
}

/// Dispatches timed together, from one queue completion to the next.
/// sg's WebGPU timestamp is an empty pass, which nothing orders after the dispatches recorded before it, so a timestamp
/// pair can close before the work it brackets has run: the wall clock is the honest measure here.
/// A completion is noticed on a tick of about 16 ms, so a batch runs long beside it: 128 of the shortest kernel here take
/// about 45 ms, which leaves a tick's rounding under a fifth of the batch and the median's well under that.
constexpr auto k_batch = 128;

/// Whether every contender writes what the first does, within float noise: each records once and its output is read back.
/// A port that drifted from the original, or a dispatch that wrote nothing, shows here rather than as a time.
/// The contenders share one target, so `clear` zeroes it before each, or one would read what the other left.
cc::shared_async<cc::unit> outputs_agree(sg::context& ctx,
                                         cc::string_view kernel,
                                         cc::span<contender> contenders,
                                         cc::function_ref<void(sg::command_list&)> clear,
                                         cc::function_ref<sg::bytes_future(sg::command_list&)> read)
{
    auto outputs = cc::vector<cc::vector<f32>>();
    for (auto& c : contenders)
    {
        auto cmd = ctx.create_command_list();
        clear(*cmd);
        c.record(*cmd);
        auto const back = read(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        auto const bytes = co_await back.bytes();
        auto values = cc::vector<f32>::create_defaulted(bytes.size() / isize(sizeof(f32)));
        cc::memcpy(values.data(), bytes.data(), values.size() * isize(sizeof(f32)));
        outputs.push_back(cc::move(values));
    }
    for (auto i = isize(0); i < outputs.size(); ++i)
    {
        auto nonzero = isize(0);
        for (auto const v : outputs[i])
            nonzero += v != 0.0f ? 1 : 0;
        // a target that stayed cleared is a dispatch that wrote nothing, which two of them would agree on
        CHECK(nonzero > 0).context(cc::format("{}: {} wrote nothing", kernel, contenders[i].name));
    }
    for (auto i = isize(1); i < outputs.size(); ++i)
    {
        REQUIRE(outputs[i].size() == outputs[0].size());
        auto worst = 0.0f;
        auto at = isize(0);
        for (auto k = isize(0); k < outputs[0].size(); ++k)
        {
            auto const a = outputs[0][k];
            auto const b = outputs[i][k];
            auto const d = (a > b ? a - b : b - a) / (1.0f + (a > 0 ? a : -a));
            if (!(d <= worst))
            {
                worst = d;
                at = k;
            }
        }
        CHECK(worst < 1e-3f)
            .context(cc::format("{}: {} against {}, worst at {}: {} and {}", kernel, contenders[i].name,
                                contenders[0].name, at, outputs[0][at], outputs[i][at]));
    }
}

/// An SGL pipeline's contender, ready once the pipeline is built.
[[nodiscard]] cc::unique_function<bool()> ready_when(sg::context& ctx, sg::async_compute_pipeline const& pipeline)
{
    return [&ctx, pipeline]
    {
        (void)ctx.backlog.start(pipeline);
        if (pipeline->has_error())
            REQUIRE(false).dump("error", cc::format("{}", pipeline->try_error()->underlying()));
        return pipeline->is_ready() && pipeline->try_value() != nullptr;
    };
}

cc::shared_async<cc::unit> compare_on_webgpu(sg::context& ctx)
{
    CC_LOG_INFO("webgpu adapter: {}, timestamps: {}", ctx.metrics.adapter().name,
                ctx.supports(sg::feature::timestamp_query) ? "yes" : "no, timing by the wall clock");

    static auto const registered = []
    {
        sr_test::shader_fixtures().add_package(sr_bench::wgsl::package());
        return true;
    }();
    (void)registered;

    ctx.upload.set_inline_budget(isize(64) << 20);
    ctx.advance_epoch();

    auto const color = co_await filled_texture(
        ctx, 1, [](i32, i32, u32 s) { return tg::vec4f(0.2f + noise(s), noise(s + 7), noise(s + 13), 1.0f); });
    auto const albedo = co_await filled_texture(
        ctx, 2, [](i32, i32, u32 s) { return tg::vec4f(0.5f, 0.6f, 0.7f, 1.0f) * (0.5f + 0.5f * noise(s)); });
    auto const normal = co_await filled_texture(
        ctx, 3, [](i32 x, i32 y, u32)
        { return tg::vec4f(f32(x % 64) / 64.0f - 0.5f, f32(y % 64) / 64.0f - 0.5f, 1.0f, 0.0f); });
    auto const depth = co_await filled_texture(
        ctx, 4, [](i32 x, i32, u32 s) { return tg::vec4f(1.0f + f32(x / 128) + 0.01f * noise(s), 0.0f, 0.0f, 0.0f); });
    auto const normal_depth = co_await filled_texture(
        ctx, 5, [](i32 x, i32 y, u32) { return tg::vec4f(0.0f, 0.0f, 1.0f, 1.0f + f32((x + y) / 128)); });
    auto const target
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                            .width = k_extent[0],
                                            .height = k_extent[1],
                                            .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                   | sg::texture_usage::copy_src | sg::texture_usage::copy_dst});
    auto const read_target = [&](sg::command_list& cmd) { return cmd.download.bytes_from_texture(target.raw()); };
    auto const zero_texels = cc::vector<tg::vec4f>::create_defaulted(isize(k_extent[0]) * k_extent[1]);
    auto const clear_target = [&](sg::command_list& cmd)
    { cmd.upload.bytes_to_texture(target.raw(), cc::span<tg::vec4f const>(zero_texels).as_bytes()); };

    // ---- à-trous, one pass at step 2 with every guide -----------------------------------------------------------------
    {
        namespace sgl = sr::sgl_shaders::atrous_denoise;
        auto const flags = (1u << 0) | (1u << 1) | (1u << 2); // albedo, normal, depth
        auto const values = sgl::options{.target_format = sg::pixel_format::rgba32_float};
        auto const layout = ctx.cached.acquire_binding_group_layout(sgl::atrous_images::declared_bindings(values),
                                                                    sgl::atrous_images::declared_samplers());
        auto const sgl_pipeline = sgl::main_cs.acquire_pipeline(ctx, values);
        auto const wgsl_layout = ctx.cached.acquire_binding_group_layout(
            wgsl_named(sgl::atrous_images::declared_bindings(values)), sgl::atrous_images::declared_samplers());
        auto wgsl = wgsl_pipeline{.shader = sr_bench::wgsl::atrous_denoise.compute.main_cs->acquire(ctx),
                                  .group_layout = wgsl_layout};
        auto const record = [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline,
                                sg::binding_group_layout_handle const& group_layout)
        {
            auto const group
                = ctx.transient.create_binding_group(cmd, group_layout,
                                                     sgl::atrous_images{.source = color.as_texture_view(),
                                                                        .albedo = albedo.as_texture_view(),
                                                                        .specular_albedo = albedo.as_texture_view(),
                                                                        .normal = normal.as_texture_view(),
                                                                        .depth = depth.as_texture_view(),
                                                                        .sample_counts = color.as_texture_view(),
                                                                        .target = target.as_any_image_view()});
            cmd.compute.bind_pipeline(pipeline);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(sgl::atrous_constants{.step = 2,
                                                                   .flags = flags,
                                                                   .luminance_scale = 0.5f,
                                                                   .normal_power = 128.0f,
                                                                   .depth_sigma = 1.0f}
                                                 .to_block());
            cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
        };
        auto contenders = cc::vector<contender>();
        contenders.push_back(
            {.name = "wgsl",
             .is_ready = [&] { return wgsl.is_ready(ctx); },
             .record = [&](sg::command_list& cmd) { record(cmd, **wgsl.pipeline->try_value(), wgsl_layout); }});
        contenders.push_back({.name = "sgl",
                              .is_ready = ready_when(ctx, sgl_pipeline),
                              .record = [&](sg::command_list& cmd) { record(cmd, **sgl_pipeline->try_value(), layout); }});
        co_await measure(ctx, "à-trous", contenders, k_batch, true);
        co_await outputs_agree(ctx, "à-trous", contenders, clear_target, read_target);
    }

    // ---- SVGF's filter, one pass at step 2 ---------------------------------------------------------------------------
    {
        namespace sgl = sr::sgl_shaders::svgf;
        auto const values = sgl::options{.target_format = sg::pixel_format::rgba32_float};
        auto const layout = ctx.cached.acquire_binding_group_layout(sgl::svgf_atrous_images::declared_bindings(values),
                                                                    sgl::svgf_atrous_images::declared_samplers());
        auto const sgl_pipeline = sgl::atrous_cs.acquire_pipeline(ctx, values);
        auto const wgsl_layout = ctx.cached.acquire_binding_group_layout(
            wgsl_named(sgl::svgf_atrous_images::declared_bindings(values)), sgl::svgf_atrous_images::declared_samplers());
        auto wgsl = wgsl_pipeline{.shader = sr_bench::wgsl::svgf_atrous.compute.atrous_cs->acquire(ctx),
                                  .group_layout = wgsl_layout};
        auto const record = [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline,
                                sg::binding_group_layout_handle const& group_layout)
        {
            auto const group = ctx.transient.create_binding_group(
                cmd, group_layout,
                sgl::svgf_atrous_images{.source = color.as_texture_view(),
                                        .normal_depth = normal_depth.as_texture_view(),
                                        .albedo = albedo.as_texture_view(),
                                        .specular_albedo = albedo.as_texture_view(),
                                        .color = color.as_texture_view(),
                                        .target = target.as_any_image_view()});
            cmd.compute.bind_pipeline(pipeline);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(sgl::svgf_atrous_constants{.step = 2,
                                                                        .flags = 0u,
                                                                        .luminance_sigma = 4.0f,
                                                                        .normal_power = 128.0f,
                                                                        .depth_sigma = 1.0f}
                                                 .to_block());
            cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
        };
        auto contenders = cc::vector<contender>();
        contenders.push_back(
            {.name = "wgsl",
             .is_ready = [&] { return wgsl.is_ready(ctx); },
             .record = [&](sg::command_list& cmd) { record(cmd, **wgsl.pipeline->try_value(), wgsl_layout); }});
        contenders.push_back({.name = "sgl",
                              .is_ready = ready_when(ctx, sgl_pipeline),
                              .record = [&](sg::command_list& cmd) { record(cmd, **sgl_pipeline->try_value(), layout); }});
        co_await measure(ctx, "svgf filter", contenders, k_batch, true);
        co_await outputs_agree(ctx, "svgf filter", contenders, clear_target, read_target);
    }

    // ---- the network's convolution, one 32-in 32-out layer over 512 x 512 -------------------------------------------
    {
        namespace sgl = sr::sgl_shaders::nn_conv;
        constexpr auto width = 512;
        constexpr auto height = 512;
        constexpr auto channels = 32;
        constexpr auto texels_per_thread = 8;

        auto features = cc::vector<f32>();
        for (auto n = 0; n < width * height * channels; ++n)
            features.push_back(noise(u32(n) * 31u + 1u) - 0.5f);
        auto weights = cc::vector<f32>();
        for (auto n = 0; n < 9 * channels * channels; ++n)
            weights.push_back((noise(u32(n) * 31u + 7u) - 0.5f) * 0.2f);
        auto const bias_offset = u32(weights.size());
        for (auto o = 0; o < channels; ++o)
            weights.push_back(noise(u32(o) + 101u) * 0.1f);

        auto const source = ctx.persistent.create_buffer<f32>(
            features.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        auto const weight_buffer = ctx.persistent.create_buffer<f32>(
            weights.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        auto const conv_target = ctx.persistent.create_buffer<f32>(features.size(), sg::buffer_usage::readwrite_buffer
                                                                                        | sg::buffer_usage::copy_src
                                                                                        | sg::buffer_usage::copy_dst);
        auto const zero_features = cc::vector<f32>::create_defaulted(features.size());
        {
            auto cmd = ctx.create_command_list();
            cmd->upload.data_to_buffer(source, features);
            cmd->upload.data_to_buffer(weight_buffer, weights);
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
        }
        auto const source4 = source.try_reinterpret_as<tg::vec4f>();
        REQUIRE(source4.has_value());

        auto const layout = ctx.cached.acquire_binding_group_layout<sgl::nn_conv_buffers>();
        auto const sgl_pipeline = sgl::main_cs.acquire_pipeline(ctx);
        auto const wgsl_layout = ctx.cached.acquire_binding_group_layout(
            wgsl_named(sgl::nn_conv_buffers::declared_bindings()), sgl::nn_conv_buffers::declared_samplers());
        auto wgsl = wgsl_pipeline{.shader = sr_bench::wgsl::nn_conv.compute.main_cs->acquire(ctx),
                                  .group_layout = wgsl_layout};
        auto const record = [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline,
                                sg::binding_group_layout_handle const& group_layout)
        {
            auto const group = ctx.transient.create_binding_group(
                cmd, group_layout,
                sgl::nn_conv_buffers{.source_a = source4.value().as_readonly_buffer(),
                                     .source_b = source4.value().as_readonly_buffer(),
                                     .weights = weight_buffer.as_readonly_buffer(),
                                     .target = conv_target.as_readwrite_buffer()});
            cmd.compute.bind_pipeline(pipeline);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(sgl::nn_conv_constants{.width = u32(width),
                                                                    .height = u32(height),
                                                                    .in_channels = u32(channels),
                                                                    .out_channels = u32(channels),
                                                                    .weight_offset = 0u,
                                                                    .bias_offset = bias_offset,
                                                                    .in_channels_a = u32(channels)}
                                                 .to_block());
            cmd.compute.dispatch_threads(channels, (width + texels_per_thread - 1) / texels_per_thread, height);
        };
        auto contenders = cc::vector<contender>();
        contenders.push_back(
            {.name = "wgsl",
             .is_ready = [&] { return wgsl.is_ready(ctx); },
             .record = [&](sg::command_list& cmd) { record(cmd, **wgsl.pipeline->try_value(), wgsl_layout); }});
        contenders.push_back({.name = "sgl",
                              .is_ready = ready_when(ctx, sgl_pipeline),
                              .record = [&](sg::command_list& cmd) { record(cmd, **sgl_pipeline->try_value(), layout); }});
        co_await measure(ctx, "convolution", contenders, k_batch, true);
        co_await outputs_agree(
            ctx, "convolution", contenders, [&](sg::command_list& cmd)
            { cmd.upload.data_to_buffer(conv_target, zero_features); }, [&](sg::command_list& cmd)
            { return cmd.download.bytes_from_buffer(conv_target.raw(), 0, conv_target.size_in_bytes()); });
    }

    co_await ctx.idle_completion();
}
} // namespace

ASYNC_BENCHMARK("sr - SGL against the same kernels written in WGSL, on webgpu")
{
    auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
    if (requested.has_error())
        SKIP("no webgpu adapter");
    static_cast<sg::backend::webgpu::webgpu_context&>(*requested.value())
        .set_message_callback([](WGPUErrorType, cc::string_view message)
                              { CHECK(false).context(cc::format("webgpu: {}", message)); });
    co_await compare_on_webgpu(*requested.value());
}
