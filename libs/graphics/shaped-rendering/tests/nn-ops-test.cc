#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// The network's other three operations: the transfer the inputs go through, the max pool and the nearest upsample.
//
// The first test here is that they COMPILE, which is not something the build says.
// A shader package generates its descriptors from the source text and compiles at run time, so an HLSL error reaches
// nobody unless something acquires the shader and reads the error back.
// A denoise member that silently declines every frame looks exactly like one that is still warming up.

namespace
{
/// Gives `lib` a compiler and sr's package, or false when there is none and the caller should skip.
[[nodiscard]] bool add_sr_shaders(slib::shader_library& lib)
{
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        return false;
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());
    return true;
}

/// Builds one compute pipeline into `out`, failing with the compiler's own text rather than a null handle.
///
/// The pipeline travels through a reference rather than the return, because what this has to report is a compiler
/// message and the coroutine's value would be the pipeline.
[[nodiscard]] cc::shared_async<cc::unit> build(sg::context& ctx,
                                               slib::shader_asset_handle const& asset,
                                               sg::binding_group_layout_handle const& layout,
                                               cc::string_view name,
                                               sg::compute_pipeline_handle& out)
{
    auto const shader = asset->acquire(ctx);
    co_await cc::async_settled(shader);
    if (shader->has_error())
        FAIL(cc::format("{} did not compile:\n{}", name, shader->try_error()->underlying().to_string()));

    auto const* const compiled = shader->try_value();
    REQUIRE(compiled != nullptr);

    auto const* const constants = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    REQUIRE(constants != nullptr).context(cc::format("{} reflects no constants block", name));

    auto pipeline = ctx.cached.acquire_compute_pipeline(
        {.shader = *compiled,
         .layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}, .inline_constants = *constants})});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr).context(cc::format("{} did not build a pipeline", name));
    out = built;
    co_return;
}
} // namespace

// Every operation the network is made of builds.
//
// Cheap, and it is the check that turns an HLSL mistake into a message here rather than into a member that declines
// every frame for a reason nothing prints.
ASYNC_INVOCABLE_TEST("sr - every network operation compiles",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    if (!add_sr_shaders(lib))
        SKIP("no DXC compiler to build the network's shaders");

    auto nn_conv_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_conv.compute.main_cs,
                   ctx.cached.acquire_binding_group_layout<sr::shaders::nn_conv_bindings>(), "nn_conv", nn_conv_pipeline);
    auto nn_input_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_input.compute.main_cs,
                   ctx.cached.acquire_binding_group_layout<sr::shaders::nn_input_bindings>(), "nn_input",
                   nn_input_pipeline);
    auto nn_output_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_output.compute.main_cs,
                   ctx.cached.acquire_binding_group_layout<sr::shaders::nn_output_bindings>(), "nn_output",
                   nn_output_pipeline);
    auto nn_pool_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_pool.compute.main_cs,
                   ctx.cached.acquire_binding_group_layout<sr::shaders::nn_pool_bindings>(), "nn_pool", nn_pool_pipeline);
    auto nn_upsample_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_upsample.compute.main_cs,
                   ctx.cached.acquire_binding_group_layout<sr::shaders::nn_upsample_bindings>(), "nn_upsample",
                   nn_upsample_pipeline);
}

// The transfer curve, through both shaders that use it.
//
// Radiance spans many orders of magnitude and the curve is what lets a network see all of it, so what matters is that
// it comes back — across the whole range, and across both of the curve's two interior knees, where a mistranscribed
// constant would show up as a step rather than as a small error.
ASYNC_INVOCABLE_TEST("sr - the network's transfer curve round-trips across the HDR range",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    if (!add_sr_shaders(lib))
        SKIP("no DXC compiler to build the network's shaders");

    auto const input_layout = ctx.cached.acquire_binding_group_layout<sr::shaders::nn_input_bindings>();
    auto const output_layout = ctx.cached.acquire_binding_group_layout<sr::shaders::nn_output_bindings>();
    auto input_pipeline = sg::compute_pipeline_handle();
    auto output_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_input.compute.main_cs, input_layout, "nn_input", input_pipeline);
    co_await build(ctx, sr::shaders::nn_output.compute.main_cs, output_layout, "nn_output", output_pipeline);

    // One texel per value, spanning nine orders of magnitude and sitting either side of both knees.
    auto const values = cc::vector<f32>{0.0f, 1e-7f, 1e-6f, 1e-4f, 1e-3f,  0.01f,   0.03f, 0.05f, 0.2f,
                                        0.5f, 1.0f,  4.0f,  20.0f, 100.0f, 1000.0f, 1e4f,  5e4f};

    auto const width = i32(values.size());
    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = width,
                                                 .height = 1,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const color = make(sg::pixel_format::rgba32_float);
    auto const albedo = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const result = make(sg::pixel_format::rgba32_float);

    auto pixels = cc::vector<tg::vec4f>();
    for (auto const v : values)
        pixels.push_back(tg::vec4f(v, v * 0.5f, v * 0.25f, 0));

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());

    auto const zeros = cc::vector<tg::vec4f>::create_filled(size_t(width), tg::vec4f(0, 0, 0, 0));
    cmd->upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(zeros).as_bytes());
    cmd->upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(zeros).as_bytes());

    // The nine-channel tensor the input shader writes, and the three-channel slice the output shader reads.
    auto const packed = ctx.transient.create_buffer<f32>(
        width * 9, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*input_pipeline);
    cmd->compute.bind<sr::shaders::nn_input_bindings>(*ctx.transient.create_binding_group(
        input_layout, sr::shaders::nn_input_bindings{.gColor = color.as_readonly_view(),
                                                     .gAlbedo = albedo.as_readonly_view(),
                                                     .gNormal = normal.as_readonly_view(),
                                                     .gTarget = packed.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(
        sr::shaders::nn_input_constants{.width = u32(width), .height = 1, .input_scale = 1.0f, ._pad = 0});
    cmd->compute.dispatch_threads(width, 1, 1);

    // The input writes nine channels and the output reads three, which in the real network is what the sixteen
    // convolutions between them do.
    // Here there are none, so the tensor comes back to the host and its first three channels go up again — the
    // shortest path to putting the two halves of the curve against each other.
    auto const readback_packed = sg::data_future<f32>(cmd->download.data_from_buffer(packed));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const encoded = co_await readback_packed.data();
    REQUIRE(encoded.size() == width * 9);

    auto compacted = cc::vector<f32>();
    for (auto i = 0; i < width; ++i)
        for (auto c = 0; c < 3; ++c)
            compacted.push_back(encoded[i * 9 + c]);

    // Created in the SECOND epoch: a transient buffer does not outlive the one it was made in, and everything above
    // ran in the first.
    auto cmd2 = ctx.create_command_list();
    auto const three
        = ctx.transient.create_buffer<f32>(width * 3, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd2->upload.data_to_buffer(three, compacted);
    cmd2->compute.bind_pipeline(*output_pipeline);
    cmd2->compute.bind<sr::shaders::nn_output_bindings>(*ctx.transient.create_binding_group(
        output_layout,
        sr::shaders::nn_output_bindings{.gSource = three.as_readonly_buffer(), .gTarget = result.as_readwrite_view()}));
    cmd2->compute.set_inline_constants(
        sr::shaders::nn_output_constants{.width = u32(width), .height = 1, .input_scale = 1.0f, ._pad = 0});
    cmd2->compute.dispatch_threads(width, 1, 1);

    auto const readback = sg::data_future<tg::vec4f>(cmd2->download.bytes_from_texture(result.raw()));
    ctx.submit_command_list(cc::move(cmd2));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == width);

    for (auto i = 0; i < width; ++i)
    {
        auto const expected = pixels[i];
        for (auto c = 0; c < 3; ++c)
        {
            // Relative, because the range spans nine orders of magnitude and an absolute bound would say nothing at
            // either end of it.
            auto const e = expected[c];
            auto const d = tg::abs(got[i][c] - e);
            CHECK(d <= 1e-3f * cc::max(e, 1e-3f)).context(cc::format("value {} came back as {}", e, got[i][c]));
        }
    }

    // The curve is not the identity, or none of the above would mean anything.
    CHECK(encoded[10 * 9 + 0] != pixels[10][0]).context("the encoded value equals the input, so no transfer was applied");
}

// The max pool and the nearest upsample, which are the only other things the network does to a feature map.
//
// Both are trivial and both are easy to get subtly wrong: a pool that takes the wrong four texels, or an upsample
// that writes three of its four, produces a feature map that is the right size and the wrong content.
ASYNC_INVOCABLE_TEST("sr - the network's pool and upsample move the texels they say they do",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    if (!add_sr_shaders(lib))
        SKIP("no DXC compiler to build the network's shaders");

    auto const pool_layout = ctx.cached.acquire_binding_group_layout<sr::shaders::nn_pool_bindings>();
    auto const up_layout = ctx.cached.acquire_binding_group_layout<sr::shaders::nn_upsample_bindings>();
    auto pool_pipeline = sg::compute_pipeline_handle();
    auto up_pipeline = sg::compute_pipeline_handle();
    co_await build(ctx, sr::shaders::nn_pool.compute.main_cs, pool_layout, "nn_pool", pool_pipeline);
    co_await build(ctx, sr::shaders::nn_upsample.compute.main_cs, up_layout, "nn_upsample", up_pipeline);

    // A 4x4 map of two channels, every value distinct so a wrong texel is a wrong number.
    constexpr auto src_w = 4;
    constexpr auto src_h = 4;
    constexpr auto channels = 2;

    auto source = cc::vector<f32>();
    for (auto y = 0; y < src_h; ++y)
        for (auto x = 0; x < src_w; ++x)
            for (auto c = 0; c < channels; ++c)
                source.push_back(f32((y * src_w + x) * channels + c));

    auto cmd = ctx.create_command_list();

    auto const src = ctx.transient.create_buffer<f32>(source.size(),
                                                      sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(src, source);

    auto const pooled = ctx.transient.create_buffer<f32>(
        (src_w / 2) * (src_h / 2) * channels,
        sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*pool_pipeline);
    cmd->compute.bind<sr::shaders::nn_pool_bindings>(*ctx.transient.create_binding_group(
        pool_layout,
        sr::shaders::nn_pool_bindings{.gSource = src.as_readonly_buffer(), .gTarget = pooled.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(
        sr::shaders::nn_pool_constants{.width = src_w / 2, .height = src_h / 2, .channels = channels, ._pad = 0});
    cmd->compute.dispatch_threads(channels, src_w / 2, src_h / 2);

    auto const up = ctx.transient.create_buffer<f32>(src_w * src_h * channels,
                                                     sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    cmd->compute.bind_pipeline(*up_pipeline);
    cmd->compute.bind<sr::shaders::nn_upsample_bindings>(*ctx.transient.create_binding_group(
        up_layout,
        sr::shaders::nn_upsample_bindings{.gSource = pooled.as_readonly_buffer(), .gTarget = up.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(
        sr::shaders::nn_upsample_constants{.width = src_w / 2, .height = src_h / 2, .channels = channels, ._pad = 0});
    cmd->compute.dispatch_threads(channels, src_w / 2, src_h / 2);

    auto const pooled_back = sg::data_future<f32>(cmd->download.data_from_buffer(pooled));
    auto const up_back = sg::data_future<f32>(cmd->download.data_from_buffer(up));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got_pool = co_await pooled_back.data();
    auto const got_up = co_await up_back.data();

    // The pool takes the maximum of the 2x2 block, which for this ramp is its bottom-right texel.
    for (auto y = 0; y < src_h / 2; ++y)
        for (auto x = 0; x < src_w / 2; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const expected = f32(((y * 2 + 1) * src_w + (x * 2 + 1)) * channels + c);
                auto const got = got_pool[(y * (src_w / 2) + x) * channels + c];
                CHECK(got == expected)
                    .context(cc::format("pool at {},{} channel {}: got {}, expected {}", x, y, c, got, expected));
            }

    // The upsample writes each pooled texel to all four under it, so every 2x2 block is uniform and equals it.
    for (auto y = 0; y < src_h; ++y)
        for (auto x = 0; x < src_w; ++x)
            for (auto c = 0; c < channels; ++c)
            {
                auto const expected = got_pool[((y / 2) * (src_w / 2) + (x / 2)) * channels + c];
                auto const got = got_up[(y * src_w + x) * channels + c];
                CHECK(got == expected).context(cc::format("upsample at {},{} channel {}", x, y, c));
            }
}
