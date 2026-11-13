#include "../shader_fixtures.hh"
#include "bench_support.hh"

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
#if SR_BENCH_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
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
using namespace sr_bench::impl;

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

/// Which two of a kernel are timed against each other.
enum class pairing : u8
{
    /// The hand-written HLSL through DXC, then SGL by the context's own route.
    hlsl_against_sgl,
    /// SGL's text through DXC, then SGL's own binary of the same source, so only the route differs.
    text_against_native,
};

/// What SGL's side of each pair wrote, which holds two routes of one source to one answer.
struct kernel_outputs
{
    cc::vector<byte> atrous;
    cc::vector<byte> svgf;
    cc::vector<f32> convolution;
};

/// The largest difference between two runs of floats, and how many differ at all.
void compare_floats(cc::string_view kernel, cc::span<f32 const> a, cc::span<f32 const> b)
{
    REQUIRE(a.size() == b.size());
    auto largest = 0.0f;
    auto differing = isize(0);
    for (auto i = isize(0); i < a.size(); ++i)
    {
        auto const difference = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        differing += a[i] == b[i] ? 0 : 1;
        largest = difference > largest ? difference : largest;
        // both NaN is agreement, which a difference of them does not say
        if (a[i] != a[i] || b[i] != b[i])
            CHECK((a[i] != a[i]) == (b[i] != b[i]));
    }
    CC_LOG_INFO("{}: {} of {} floats differ, by at most {}", kernel, differing, a.size(), largest);
    // DXC and SGL contract and reorder differently under `fast`, so a few ulps of each pass through a kernel are allowed
    CHECK(largest < 1e-3f);
}
} // namespace

/// Every pair on `ctx`: the HLSL through DXC to DXIL or SPIR-V, beside SGL's text through the same compiler.
/// `outputs`, where given, takes what SGL's side wrote.
static cc::shared_async<cc::unit> compare_on(sg::context& ctx,
                                             kernel_outputs* outputs = nullptr,
                                             pairing pair = pairing::hlsl_against_sgl)
{
    CC_LOG_INFO("{} adapter: {}", ctx.backend() == sg::backend_kind::vulkan ? "vulkan" : "dx12",
                ctx.metrics.adapter().name);

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
        auto const native_pipeline
            = slib::acquire_compute_pipeline(&ctx, sr::sgl_shaders::atrous_denoise::main_cs.asset,
                                             sr::sgl_shaders::atrous_denoise::main_cs.acquire_layout(ctx, sgl_values),
                                             with_native(ctx, sgl_values.values()));
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
        auto record_sgl = cc::unique_function<void(sg::command_list&, sg::compute_pipeline const&)>(
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, sgl_layout,
                    sr::sgl_shaders::atrous_denoise::atrous_images{.source = color.as_texture_view(),
                                                                   .albedo = albedo.as_texture_view(),
                                                                   .specular_albedo = albedo.as_texture_view(),
                                                                   .normal = normal.as_texture_view(),
                                                                   .depth = depth.as_texture_view(),
                                                                   .sample_counts = color.as_texture_view(),
                                                                   .input = color.as_texture_view(),
                                                                   .target = target.as_any_image_view()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(
                    sr::sgl_shaders::atrous_denoise::atrous_constants{.step = 2,
                                                                      .flags = flags,
                                                                      .luminance_scale = 0.5f,
                                                                      .normal_power = 128.0f,
                                                                      .depth_sigma = 1.0f,
                                                                      .ceiling = 3.4028234e38f}
                        .to_block());
                cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
            });
        if (pair == pairing::text_against_native)
        {
            contenders.clear();
            contenders.push_back(contender_of("text", ctx, sgl_pipeline, &record_sgl));
            contenders.push_back(contender_of("native", ctx, native_pipeline, &record_sgl));
        }
        else
            contenders.push_back(contender_of("sgl", ctx, sgl_pipeline, &record_sgl));
        co_await measure(ctx, "à-trous", contenders);
        if (outputs != nullptr)
        {
            auto cmd = ctx.create_command_list();
            contenders[1].record(*cmd);
            auto const written = cmd->download.bytes_from_texture(target.raw());
            ctx.submit_command_list(cc::move(cmd));
            auto const data = co_await written.bytes();
            outputs->atrous = cc::vector<byte>::create_copy_of(cc::span<byte const>(data));
        }
    }

    // ---- SVGF's filter, one pass at step 2 ---------------------------------------------------------------------------
    {
        auto const sgl_values = sr::sgl_shaders::svgf::options{.target_format = sg::pixel_format::rgba32_float};
        auto const sgl_layout = ctx.cached.acquire_binding_group_layout(
            sr::sgl_shaders::svgf::svgf_atrous_images::declared_bindings(sgl_values),
            sr::sgl_shaders::svgf::svgf_atrous_images::declared_samplers());
        auto const sgl_pipeline = sr::sgl_shaders::svgf::atrous_cs.acquire_pipeline(ctx, sgl_values);
        auto const native_pipeline = slib::acquire_compute_pipeline(
            &ctx, sr::sgl_shaders::svgf::atrous_cs.asset,
            sr::sgl_shaders::svgf::atrous_cs.acquire_layout(ctx, sgl_values), with_native(ctx, sgl_values.values()));
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
        auto record_sgl = cc::unique_function<void(sg::command_list&, sg::compute_pipeline const&)>(
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, sgl_layout,
                    sr::sgl_shaders::svgf::svgf_atrous_images{.source = color.as_texture_view(),
                                                              .normal_depth = normal_depth.as_texture_view(),
                                                              .albedo = albedo.as_texture_view(),
                                                              .specular_albedo = albedo.as_texture_view(),
                                                              .color = color.as_texture_view(),
                                                              .target = target.as_any_image_view()});
                cmd.compute.bind_pipeline(pipeline);
                cmd.compute.bind_group(0, *group);
                cmd.compute.set_inline_constants(sr::sgl_shaders::svgf::svgf_atrous_constants{.step = 2,
                                                                                              .flags = 0u,
                                                                                              .luminance_sigma = 4.0f,
                                                                                              .normal_power = 128.0f,
                                                                                              .depth_sigma = 1.0f,
                                                                                              .ceiling = 3.4028234e38f}
                                                     .to_block());
                cmd.compute.dispatch_threads(k_extent[0], k_extent[1], 1);
            });
        if (pair == pairing::text_against_native)
        {
            contenders.clear();
            contenders.push_back(contender_of("text", ctx, sgl_pipeline, &record_sgl));
            contenders.push_back(contender_of("native", ctx, native_pipeline, &record_sgl));
        }
        else
            contenders.push_back(contender_of("sgl", ctx, sgl_pipeline, &record_sgl));
        co_await measure(ctx, "svgf filter", contenders);
        if (outputs != nullptr)
        {
            auto cmd = ctx.create_command_list();
            contenders[1].record(*cmd);
            auto const written = cmd->download.bytes_from_texture(target.raw());
            ctx.submit_command_list(cc::move(cmd));
            auto const data = co_await written.bytes();
            outputs->svgf = cc::vector<byte>::create_copy_of(cc::span<byte const>(data));
        }
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
        auto const conv_target = ctx.persistent.create_buffer<f32>(
            features.size(), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
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
        auto const sgl_shader = co_await sr::sgl_shaders::nn_conv::main_cs->acquire(ctx);
        CC_LOG_INFO("the convolution's SGL came through {}", sgl_shader.compiler.name);
        auto const sgl_pipeline = sr::sgl_shaders::nn_conv::main_cs.acquire_pipeline(ctx);
        auto const native_pipeline = slib::acquire_compute_pipeline(
            &ctx, sr::sgl_shaders::nn_conv::main_cs.asset, sr::sgl_shaders::nn_conv::main_cs.acquire_layout(ctx),
            with_native(ctx, {}));
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
        auto record_sgl = cc::unique_function<void(sg::command_list&, sg::compute_pipeline const&)>(
            [&](sg::command_list& cmd, sg::compute_pipeline const& pipeline)
            {
                auto const group = ctx.transient.create_binding_group(
                    cmd, sgl_layout,
                    sr::sgl_shaders::nn_conv::nn_conv_buffers{.source_a = source4.value().as_readonly_buffer(),
                                                              .source_b = source4.value().as_readonly_buffer(),
                                                              .weights = weight_buffer.as_readonly_buffer(),
                                                              .target = conv_target.as_readwrite_buffer()});
                cmd.compute.bind_pipeline(pipeline);
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
            });
        if (pair == pairing::text_against_native)
        {
            contenders.clear();
            contenders.push_back(contender_of("text", ctx, sgl_pipeline, &record_sgl));
            contenders.push_back(contender_of("native", ctx, native_pipeline, &record_sgl));
        }
        else
            contenders.push_back(contender_of("sgl", ctx, sgl_pipeline, &record_sgl));
        co_await measure(ctx, "convolution", contenders);
        if (outputs != nullptr)
        {
            auto cmd = ctx.create_command_list();
            contenders[1].record(*cmd);
            auto const written = cmd->download.data_from_buffer(conv_target);
            ctx.submit_command_list(cc::move(cmd));
            auto const data = co_await written.data();
            outputs->convolution = cc::vector<f32>::create_copy_of(cc::span<f32 const>(data));
        }
    }

    co_await ctx.idle_completion();
}

ASYNC_BENCHMARK("sr - SGL against the HLSL it was ported from, GPU time per dispatch")
{
    auto created = sg::create_dx12_context({.native_dxil = false, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    co_await compare_on(*created.value());
}

// The same kernels with SGL's own DXIL against DXC's of the hand-written HLSL, on a device that loads it unsigned.
ASYNC_BENCHMARK("sr - SGL's own DXIL against the HLSL it was ported from, GPU time per dispatch, on dx12")
{
    auto created = sg::create_dx12_context({.native_dxil = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    co_await compare_on(*created.value());
}

// One SGL source through both routes, on one device: an answer of its own, beside the speed above.
ASYNC_BENCHMARK("sr - SGL's own DXIL computes what its text through DXC computes, on dx12")
{
    auto through_dxc = kernel_outputs();
    auto native = kernel_outputs();
    {
        auto created
            = sg::create_dx12_context({.native_dxil = false, .adapter = sg::backend::dx12::dx12_adapter::hardware});
        if (created.has_error())
            SKIP("no dx12 hardware device");
        created.value()->download.set_budget(isize(128) << 20);
        co_await compare_on(*created.value(), &through_dxc);
    }
    {
        auto created
            = sg::create_dx12_context({.native_dxil = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
        REQUIRE(created.has_value());
        created.value()->download.set_budget(isize(128) << 20);
        co_await compare_on(*created.value(), &native);
    }
    auto const floats = [](cc::vector<byte> const& bytes)
    { return cc::span<f32 const>(reinterpret_cast<f32 const*>(bytes.data()), bytes.size() / isize(sizeof(f32))); };
    compare_floats("à-trous", floats(through_dxc.atrous), floats(native.atrous));
    compare_floats("svgf filter", floats(through_dxc.svgf), floats(native.svgf));
    compare_floats("convolution", through_dxc.convolution, native.convolution);
}

// One SGL source, its text through DXC against SGL's own binary, interleaved on one device: the parity the native route
// is held to (libs/graphics/shaped-graphics-language/docs/native-parity.md).
ASYNC_BENCHMARK("sr - SGL's own binary against its text through DXC, GPU time per dispatch, on dx12")
{
    auto created = sg::create_dx12_context({.native_dxil = false, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (created.has_error())
        SKIP("no dx12 hardware device");
    co_await compare_on(*created.value(), nullptr, pairing::text_against_native);
}

#if SR_BENCH_HAS_VULKAN
ASYNC_BENCHMARK("sr - SGL's own binary against its text through DXC, GPU time per dispatch, on vulkan")
{
    // what the driver made of each route's pipeline, beside the time it takes
    auto created = sg::create_vulkan_context({.native_spirv = false, .capture_driver_statistics = true});
    if (created.has_error())
        SKIP("no vulkan device");
    co_await compare_on(*created.value(), nullptr, pairing::text_against_native);
}

ASYNC_BENCHMARK("sr - SGL against the HLSL it was ported from, GPU time per dispatch, on vulkan")
{
    auto created = sg::create_vulkan_context({.native_spirv = false});
    if (created.has_error())
        SKIP("no vulkan device");
    co_await compare_on(*created.value());
}

// The same kernels with SGL's own SPIR-V against DXC's of the hand-written HLSL, which is what DXC's optimizer is worth.
ASYNC_BENCHMARK("sr - SGL's own SPIR-V against the HLSL it was ported from, GPU time per dispatch, on vulkan")
{
    auto created = sg::create_vulkan_context({.native_spirv = true});
    if (created.has_error())
        SKIP("no vulkan device");
    co_await compare_on(*created.value());
}
#endif
