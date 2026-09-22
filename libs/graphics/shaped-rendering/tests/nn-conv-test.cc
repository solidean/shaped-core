#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include "shader_fixtures.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// The 3x3 convolution the denoise network is built from, against an independent implementation of the same thing.
//
// Sixteen of these ARE the network, so everything downstream rests on this one shader: a transposed weight index, an
// off-by-one in the padding or a channel stride read the wrong way round all produce a plausible image that is not
// the one the trained weights describe.
// None of that shows up as an error, and once the whole U-Net is running none of it is localizable either — which is
// why this is checked here, on shapes small enough to compute by hand.
//
// The reference below is deliberately the naive, obvious loop nest, written from the definition rather than from the
// shader.
// The shader is the one that reorders anything, so the two agreeing means the reordering is what it claims.

namespace
{
constexpr int k_width = 7;
constexpr int k_height = 5;
constexpr int k_in = 8; // a multiple of four, because the shader reads four channels in one load
constexpr int k_out = 4;

/// Deterministic, and spread over positive and negative so a dropped sign does not cancel out.
[[nodiscard]] f32 sample_value(int seed)
{
    return f32((seed * 37 % 71) - 35) * 0.031f;
}

/// The convolution, written from its definition: zero padding, 3x3, bias, ReLU.
/// Feature maps are HWC and the weights are [ky][kx][i][o], which is the contract the shader also holds.
/// The output channel is innermost because it is what varies across a wave, and the shader's loads have to coalesce.
[[nodiscard]] cc::vector<f32> reference_conv(cc::span<f32 const> source,
                                             cc::span<f32 const> weights,
                                             cc::span<f32 const> bias)
{
    auto out = cc::vector<f32>::create_filled(size_t(k_width * k_height * k_out), 0.0f);

    for (auto y = 0; y < k_height; ++y)
        for (auto x = 0; x < k_width; ++x)
            for (auto o = 0; o < k_out; ++o)
            {
                auto sum = bias[o];
                for (auto ky = -1; ky <= 1; ++ky)
                    for (auto kx = -1; kx <= 1; ++kx)
                    {
                        auto const sx = x + kx;
                        auto const sy = y + ky;
                        if (sx < 0 || sy < 0 || sx >= k_width || sy >= k_height)
                            continue; // zero padding

                        auto const tap = ((ky + 1) * 3 + (kx + 1));
                        for (auto i = 0; i < k_in; ++i)
                        {
                            auto const w = weights[(tap * k_in + i) * k_out + o];
                            sum += w * source[(sy * k_width + sx) * k_in + i];
                        }
                    }
                out[(y * k_width + x) * k_out + o] = cc::max(sum, 0.0f);
            }

    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - the network's convolution matches a reference implementation",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const shader = sr::shaders::nn_conv.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(shader);
    if (shader->has_error())
        FAIL(cc::format("nn_conv did not compile:\n{}", shader->try_error()->underlying().to_string()));
    auto const* const compiled = shader->try_value();
    REQUIRE(compiled != nullptr);

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    REQUIRE(constants_binding != nullptr);

    auto const group_layout = ctx.cached.acquire_binding_group_layout<sr::shaders::nn_conv_bindings>();
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}, .inline_constants = *constants_binding});
    auto pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    auto const built = co_await pipeline;
    REQUIRE(built != nullptr);

    // The inputs, filled so that no two positions share a value — a stride read the wrong way round then lands on a
    // different number rather than on a coincidence.
    auto source = cc::vector<f32>();
    for (auto n = 0; n < k_width * k_height * k_in; ++n)
        source.push_back(sample_value(n + 1));

    auto weights = cc::vector<f32>();
    for (auto n = 0; n < k_out * 9 * k_in; ++n)
        weights.push_back(sample_value(n + 13) * 0.4f);

    auto bias = cc::vector<f32>();
    for (auto o = 0; o < k_out; ++o)
        bias.push_back(sample_value(o + 101) * 0.2f);

    // One buffer carries the weights and the bias, as the network's does.
    auto packed = weights;
    for (auto const b : bias)
        packed.push_back(b);

    auto cmd = ctx.create_command_list();

    auto const source_buffer = ctx.transient.create_buffer<f32>(
        source.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(source_buffer, source);

    auto const weight_buffer = ctx.transient.create_buffer<f32>(
        packed.size(), sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(weight_buffer, packed);

    auto const target_buffer = ctx.transient.create_buffer<f32>(
        k_width * k_height * k_out, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    // The shader reads its sources four channels at a time, so they are bound as a float4 view of the same memory.
    auto const source4 = source_buffer.try_reinterpret_as<tg::vec4f>();
    REQUIRE(source4.has_value());

    auto const group = ctx.transient.create_binding_group(
        group_layout, sr::shaders::nn_conv_bindings{.gSourceA = source4.value().as_readonly_buffer(),
                                                    // Nothing reads B here: every channel is below in_channels_a.
                                                    .gSourceB = source4.value().as_readonly_buffer(),
                                                    .gWeights = weight_buffer.as_readonly_buffer(),
                                                    .gTarget = target_buffer.as_readwrite_buffer()});

    auto const constants = sr::shaders::nn_conv_constants{
        .width = u32(k_width),
        .height = u32(k_height),
        .in_channels = u32(k_in),
        .out_channels = u32(k_out),
        .weight_offset = 0,
        .bias_offset = u32(weights.size()),
        .in_channels_a = u32(k_in),
        ._pad = 0,
    };

    cmd->compute.bind_pipeline(*built);
    cmd->compute.bind<sr::shaders::nn_conv_bindings>(*group);
    cmd->compute.set_inline_constants(constants);
    cmd->compute.dispatch_threads(k_out, k_width, k_height);

    auto const readback = sg::data_future<f32>(cmd->download.data_from_buffer(target_buffer));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_width * k_height * k_out);

    auto const expected = reference_conv(source, weights, bias);

    auto worst = 0.0f;
    auto worst_at = 0;
    for (auto n = 0; n < k_width * k_height * k_out; ++n)
    {
        auto const d = tg::abs(got[n] - expected[n]);
        if (d > worst)
        {
            worst = d;
            worst_at = n;
        }
    }

    CHECK(worst < 1e-5f)
        .context(cc::format("worst difference {} at element {} (got {}, expected {})", worst, worst_at, got[worst_at],
                            expected[worst_at]));

    // The ReLU actually clipped something, or the comparison above would hold just as well without it.
    auto clipped = 0;
    for (auto const v : expected)
        if (v == 0.0f)
            ++clipped;
    CHECK(clipped > 0).context("no output was negative before the ReLU, so this says nothing about it");
}
