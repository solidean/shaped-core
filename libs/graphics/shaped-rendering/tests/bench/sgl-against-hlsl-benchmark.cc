#include "../shader_fixtures.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-library/shader_library.hh>
#include <sr_bench_hlsl_shaders.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// What sr ships in SGL against the hand-written HLSL each shader was ported from, in GPU time per dispatch.
//
// The HLSL under tests/bench/shaders is the shader as it stood before 733d3c4ed ported it, so each pair is one
// kernel written twice by the same hand, which is the comparison readme.md's performance aspiration is about.
// Both read the same inputs and write a target of the same format, and are dispatched with the same grid.
//
// The two of a pair alternate dispatch by dispatch, each bracketed by its own timestamps, so a GPU whose clocks drift
// under load treats them alike, and neither the CPU's recording nor the gaps between submissions count.
// A benchmark rather than a test: it asserts nothing, and logs each pair's medians and their ratio.

namespace
{
constexpr auto k_extent = tg::vec2i(1024, 1024);
constexpr int k_rounds = 64;

/// One side of a pair: what it is called, and what records one dispatch of it, ready once its pipeline is.
struct contender
{
    cc::string name;
    cc::unique_function<bool()> is_ready;
    cc::unique_function<void(sg::command_list&)> record;
};

/// The GPU milliseconds of each span, sorted.
[[nodiscard]] cc::vector<f64> sorted_ms(cc::span<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp> const> spans)
{
    auto ms = cc::vector<f64>();
    for (auto const& [from, to] : spans)
        ms.push_back(1e3 * (to.try_get_seconds().value() - from.try_get_seconds().value()));
    cc::sort(ms);
    return ms;
}

/// A value of `n` spread over [0, 1) by a fixed hash, so every run sees the same inputs.
[[nodiscard]] f32 noise(u32 n)
{
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    n ^= n >> 16;
    return f32(n >> 8) / f32(1u << 24);
}

/// An rgba32_float texture of `k_extent`, filled from `texel` and uploaded on a list of its own.
cc::shared_async<sg::texture_2d> filled_texture(sg::context& ctx, u32 seed, cc::function_ref<tg::vec4f(i32, i32, u32)> texel)
{
    auto texels = cc::vector<tg::vec4f>();
    texels.reserve(isize(k_extent[0]) * k_extent[1]);
    for (auto y = 0; y < k_extent[1]; ++y)
        for (auto x = 0; x < k_extent[0]; ++x)
            texels.push_back(texel(x, y, seed * 0x9e3779b9u + u32(y * k_extent[0] + x) * 31u));
    auto t = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                               .width = k_extent[0],
                                               .height = k_extent[1],
                                               .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(texels).as_bytes());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_return t;
}

[[nodiscard]] sg::texture_2d target_texture(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                             .width = k_extent[0],
                                             .height = k_extent[1],
                                             .usage = sg::texture_usage::texture | sg::texture_usage::image});
}

/// The pipeline of a compiled HLSL shader under its one group and its reflected constants, once both have built.
/// Empty while either is still building, and stays empty for one that failed, which `is_ready` then never reports.
template <class Group>
struct hlsl_pipeline
{
    sg::async_compiled_shader shader;
    sg::binding_group_layout_handle group_layout;
    sg::async_compute_pipeline pipeline;

    [[nodiscard]] bool is_ready(sg::context& ctx)
    {
        if (pipeline.is_valid())
        {
            (void)ctx.backlog.start(pipeline);
            return pipeline->is_ready() && pipeline->try_value() != nullptr;
        }
        (void)ctx.backlog.start(shader);
        if (!shader->is_ready() || shader->try_value() == nullptr)
            return false;
        auto const* const compiled = shader->try_value();
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::constants_buffer)
            {
                group_layout = ctx.cached.acquire_binding_group_layout<Group>();
                auto const layout = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}, .inline_constants = b});
                pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
            }
        return false;
    }
};

/// Records `rounds` dispatches of each contender, alternating, and logs what they took.
cc::shared_async<cc::unit> measure(sg::context& ctx, cc::string_view kernel, cc::span<contender> contenders)
{
    // Untimed until every contender has executed once, which is what its shaders compiling takes.
    auto const start = cc::current_time_steady_secs();
    for (auto& c : contenders)
    {
        while (!c.is_ready())
        {
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            (void)ctx.routines.tick();
            ctx.advance_epoch();
            co_await ctx.idle_completion();
        }
        auto cmd = ctx.create_command_list();
        c.record(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }

    auto spans = cc::vector<cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>>();
    for (auto i = isize(0); i < contenders.size(); ++i)
        spans.push_back({});
    for (auto r = 0; r < k_rounds; ++r)
        for (auto i = isize(0); i < contenders.size(); ++i)
        {
            auto cmd = ctx.create_command_list();
            auto const from = cmd->query.record_gpu_timestamp();
            contenders[i].record(*cmd);
            auto const to = cmd->query.record_gpu_timestamp();
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            spans[i].push_back({from, to});
        }
    co_await ctx.idle_completion();

    auto const baseline = sorted_ms(spans[0]);
    for (auto i = isize(0); i < contenders.size(); ++i)
    {
        auto const ms = sorted_ms(spans[i]);
        auto const median = ms[ms.size() / 2];
        CC_LOG_INFO("{}: {} median {} ms, min {} ms, {}x {}", kernel, contenders[i].name, median, ms[0],
                    median / baseline[baseline.size() / 2], contenders[0].name);
    }
}
} // namespace

ASYNC_BENCHMARK("sr - SGL against the HLSL it was ported from, GPU time per dispatch")
{
    auto created = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    auto& ctx = *created.value();
    CC_LOG_INFO("adapter: {}", ctx.metrics.adapter().name);

    // The fixture's library holds sr's own packages; the HLSL originals join it here, in the one binary that has them.
    static auto const registered = []
    {
        sr_test::shader_fixtures().add_package(sr_bench::hlsl::package());
        return true;
    }();
    (void)registered;

    // The largest single upload is the convolution's feature map, which the default inline ring would split off.
    ctx.upload.set_inline_budget(isize(64) << 20);
    ctx.advance_epoch();

    auto const color = co_await filled_texture(
        ctx, 1, [](i32, i32, u32 s) { return tg::vec4f(0.2f + noise(s), noise(s + 7), noise(s + 13), 1.0f); });
    auto const albedo = co_await filled_texture(
        ctx, 2, [](i32, i32, u32 s) { return tg::vec4f(0.5f, 0.6f, 0.7f, 1.0f) * (0.5f + 0.5f * noise(s)); });
    // a normal that turns slowly across the image, so the edge-stops pass some taps and reject others
    auto const normal = co_await filled_texture(
        ctx, 3, [](i32 x, i32 y, u32)
        { return tg::vec4f(f32(x % 64) / 64.0f - 0.5f, f32(y % 64) / 64.0f - 0.5f, 1.0f, 0.0f); });
    auto const depth = co_await filled_texture(
        ctx, 4, [](i32 x, i32, u32 s) { return tg::vec4f(1.0f + f32(x / 128) + 0.01f * noise(s), 0.0f, 0.0f, 0.0f); });
    auto const normal_depth = co_await filled_texture(
        ctx, 5, [](i32 x, i32 y, u32) { return tg::vec4f(0.0f, 0.0f, 1.0f, 1.0f + f32((x + y) / 128)); });
    auto const target = target_texture(ctx);

    // ---- à-trous, one pass at step 2 with every guide -----------------------------------------------------------------
    {
        auto const flags = (1u << 0) | (1u << 1) | (1u << 2); // albedo, normal, depth
        auto const sgl_values = sr::sgl_shaders::atrous_denoise::options{.target_format = sg::pixel_format::rgba32_float};
        auto const sgl_layout = ctx.cached.acquire_binding_group_layout(
            sr::sgl_shaders::atrous_denoise::atrous_images::declared_bindings(sgl_values),
            sr::sgl_shaders::atrous_denoise::atrous_images::declared_samplers());
        auto const sgl_pipeline = sr::sgl_shaders::atrous_denoise::main_cs.acquire_pipeline(ctx, sgl_values);
        auto hlsl = hlsl_pipeline<sr_bench::hlsl::atrous_bindings>{
            .shader = sr_bench::hlsl::atrous_denoise.compute.main_cs->acquire(ctx)};

        auto contenders = cc::vector<contender>();
        contenders.push_back({.name = "hlsl",
                              .is_ready = [&] { return hlsl.is_ready(ctx); },
                              .record =
                                  [&](sg::command_list& cmd)
                              {
                                  auto const group = ctx.transient.create_binding_group(
                                      cmd, hlsl.group_layout,
                                      sr_bench::hlsl::atrous_bindings{.gSource = color.as_texture_view(),
                                                                      .gAlbedo = albedo.as_texture_view(),
                                                                      .gSpecularAlbedo = albedo.as_texture_view(),
                                                                      .gNormal = normal.as_texture_view(),
                                                                      .gDepth = depth.as_texture_view(),
                                                                      .gSampleCounts = color.as_texture_view(),
                                                                      .gTarget = target.as_any_image_view()});
                                  cmd.compute.bind_pipeline(**hlsl.pipeline->try_value());
                                  cmd.compute.bind<sr_bench::hlsl::atrous_bindings>(*group);
                                  cmd.compute.set_inline_constants(
                                      sr_bench::hlsl::atrous_constants{.step = 2,
                                                                       .flags = flags,
                                                                       .luminance_scale = 0.5f,
                                                                       .normal_power = 128.0f,
                                                                       .depth_sigma = 1.0f});
                                  cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
                              }});
        contenders.push_back(
            {.name = "sgl",
             .is_ready =
                 [&]
             {
                 (void)ctx.backlog.start(sgl_pipeline);
                 return sgl_pipeline->is_ready() && sgl_pipeline->try_value() != nullptr;
             },
             .record =
                 [&](sg::command_list& cmd)
             {
                 auto const group = ctx.transient.create_binding_group(
                     cmd, sgl_layout,
                     sr::sgl_shaders::atrous_denoise::atrous_images{.source = color.as_texture_view(),
                                                                    .albedo = albedo.as_texture_view(),
                                                                    .specular_albedo = albedo.as_texture_view(),
                                                                    .normal = normal.as_texture_view(),
                                                                    .depth = depth.as_texture_view(),
                                                                    .sample_counts = color.as_texture_view(),
                                                                    .target = target.as_any_image_view()});
                 cmd.compute.bind_pipeline(**sgl_pipeline->try_value());
                 cmd.compute.bind_group(0, *group);
                 cmd.compute.set_inline_constants(
                     sr::sgl_shaders::atrous_denoise::atrous_constants{.step = 2,
                                                                       .flags = flags,
                                                                       .luminance_scale = 0.5f,
                                                                       .normal_power = 128.0f,
                                                                       .depth_sigma = 1.0f}
                         .to_block());
                 cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
             }});
        co_await measure(ctx, "à-trous", contenders);
    }

    // ---- SVGF's filter, one pass at step 2 ---------------------------------------------------------------------------
    {
        auto const sgl_values = sr::sgl_shaders::svgf::options{.target_format = sg::pixel_format::rgba32_float};
        auto const sgl_layout = ctx.cached.acquire_binding_group_layout(
            sr::sgl_shaders::svgf::svgf_atrous_images::declared_bindings(sgl_values),
            sr::sgl_shaders::svgf::svgf_atrous_images::declared_samplers());
        auto const sgl_pipeline = sr::sgl_shaders::svgf::atrous_cs.acquire_pipeline(ctx, sgl_values);
        auto hlsl = hlsl_pipeline<sr_bench::hlsl::svgf_atrous_bindings>{
            .shader = sr_bench::hlsl::svgf_atrous.compute.main_cs->acquire(ctx)};

        auto contenders = cc::vector<contender>();
        contenders.push_back(
            {.name = "hlsl",
             .is_ready = [&] { return hlsl.is_ready(ctx); },
             .record =
                 [&](sg::command_list& cmd)
             {
                 auto const group = ctx.transient.create_binding_group(
                     cmd, hlsl.group_layout,
                     sr_bench::hlsl::svgf_atrous_bindings{.gSource = color.as_texture_view(),
                                                          .gNormalDepth = normal_depth.as_texture_view(),
                                                          .gAlbedo = albedo.as_texture_view(),
                                                          .gSpecularAlbedo = albedo.as_texture_view(),
                                                          .gColor = color.as_texture_view(),
                                                          .gTarget = target.as_any_image_view()});
                 cmd.compute.bind_pipeline(**hlsl.pipeline->try_value());
                 cmd.compute.bind<sr_bench::hlsl::svgf_atrous_bindings>(*group);
                 cmd.compute.set_inline_constants(sr_bench::hlsl::svgf_atrous_constants{.step = 2,
                                                                                        .flags = 0u,
                                                                                        .luminance_sigma = 4.0f,
                                                                                        .normal_power = 128.0f,
                                                                                        .depth_sigma = 1.0f});
                 cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
             }});
        contenders.push_back(
            {.name = "sgl",
             .is_ready =
                 [&]
             {
                 (void)ctx.backlog.start(sgl_pipeline);
                 return sgl_pipeline->is_ready() && sgl_pipeline->try_value() != nullptr;
             },
             .record =
                 [&](sg::command_list& cmd)
             {
                 auto const group = ctx.transient.create_binding_group(
                     cmd, sgl_layout,
                     sr::sgl_shaders::svgf::svgf_atrous_images{.source = color.as_texture_view(),
                                                               .normal_depth = normal_depth.as_texture_view(),
                                                               .albedo = albedo.as_texture_view(),
                                                               .specular_albedo = albedo.as_texture_view(),
                                                               .color = color.as_texture_view(),
                                                               .target = target.as_any_image_view()});
                 cmd.compute.bind_pipeline(**sgl_pipeline->try_value());
                 cmd.compute.bind_group(0, *group);
                 cmd.compute.set_inline_constants(sr::sgl_shaders::svgf::svgf_atrous_constants{.step = 2,
                                                                                               .flags = 0u,
                                                                                               .luminance_sigma = 4.0f,
                                                                                               .normal_power = 128.0f,
                                                                                               .depth_sigma = 1.0f}
                                                      .to_block());
                 cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
             }});
        co_await measure(ctx, "svgf filter", contenders);
    }

    // ---- the network's convolution, one 32-in 32-out layer over 512 x 512 -------------------------------------------
    {
        constexpr auto width = 512;
        constexpr auto height = 512;
        constexpr auto channels = 32;
        // as nn_conv.sgl's `conv_texels` and the HLSL's NN_CONV_TEXELS, which agree
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
        auto const conv_target = ctx.persistent.create_buffer<f32>(features.size(), sg::buffer_usage::readwrite_buffer);
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

        auto const sgl_layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::nn_conv::nn_conv_buffers>();
        auto const sgl_pipeline = sr::sgl_shaders::nn_conv::main_cs.acquire_pipeline(ctx);
        auto hlsl = hlsl_pipeline<sr_bench::hlsl::nn_conv_bindings>{
            .shader = sr_bench::hlsl::nn_conv.compute.main_cs->acquire(ctx)};
        auto const dispatch = [&](sg::command_list& cmd)
        { cmd.compute.dispatch_threads(channels, (width + texels_per_thread - 1) / texels_per_thread, height); };

        auto contenders = cc::vector<contender>();
        contenders.push_back({.name = "hlsl",
                              .is_ready = [&] { return hlsl.is_ready(ctx); },
                              .record =
                                  [&](sg::command_list& cmd)
                              {
                                  auto const group = ctx.transient.create_binding_group(
                                      cmd, hlsl.group_layout,
                                      sr_bench::hlsl::nn_conv_bindings{.gSourceA = source4.value().as_readonly_buffer(),
                                                                       .gSourceB = source4.value().as_readonly_buffer(),
                                                                       .gWeights = weight_buffer.as_readonly_buffer(),
                                                                       .gTarget = conv_target.as_readwrite_buffer()});
                                  cmd.compute.bind_pipeline(**hlsl.pipeline->try_value());
                                  cmd.compute.bind<sr_bench::hlsl::nn_conv_bindings>(*group);
                                  cmd.compute.set_inline_constants(
                                      sr_bench::hlsl::nn_conv_constants{.width = u32(width),
                                                                        .height = u32(height),
                                                                        .in_channels = u32(channels),
                                                                        .out_channels = u32(channels),
                                                                        .weight_offset = 0u,
                                                                        .bias_offset = bias_offset,
                                                                        .in_channels_a = u32(channels)});
                                  dispatch(cmd);
                              }});
        contenders.push_back(
            {.name = "sgl",
             .is_ready =
                 [&]
             {
                 (void)ctx.backlog.start(sgl_pipeline);
                 return sgl_pipeline->is_ready() && sgl_pipeline->try_value() != nullptr;
             },
             .record =
                 [&](sg::command_list& cmd)
             {
                 auto const group = ctx.transient.create_binding_group(
                     cmd, sgl_layout,
                     sr::sgl_shaders::nn_conv::nn_conv_buffers{.source_a = source4.value().as_readonly_buffer(),
                                                               .source_b = source4.value().as_readonly_buffer(),
                                                               .weights = weight_buffer.as_readonly_buffer(),
                                                               .target = conv_target.as_readwrite_buffer()});
                 cmd.compute.bind_pipeline(**sgl_pipeline->try_value());
                 cmd.compute.bind_group(0, *group);
                 cmd.compute.set_inline_constants(
                     sr::sgl_shaders::nn_conv::nn_conv_constants{.width = u32(width),
                                                                 .height = u32(height),
                                                                 .in_channels = u32(channels),
                                                                 .out_channels = u32(channels),
                                                                 .weight_offset = 0u,
                                                                 .bias_offset = bias_offset,
                                                                 .in_channels_a = u32(channels)}
                         .to_block());
                 dispatch(cmd);
             }});
        co_await measure(ctx, "convolution", contenders);
    }

    co_await ctx.idle_completion();
}
