#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// The whole U-Net: nine packed channels in, sixteen convolutions, four pools, four upsamples, three channels out.
//
// What this can check on its own is that the graph HOLDS TOGETHER — every layer finds weights of the width its
// source produces, every skip concatenation adds up, and the result is finite radiance rather than a NaN that one
// mismatched stride would spread across the whole image.
// Whether it computes what Intel's implementation computes is a different question, and the answer to it is a
// comparison against OIDN's own filter rather than anything assertable here.

ASYNC_INVOCABLE_TEST("sr - the denoise network runs end to end",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the network's shaders");
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());

    // Small, and a multiple of sixteen because four pools halve it four times.
    // The naive convolution is quadratic in the channel count, so a larger image here would be a slow test rather
    // than a better one.
    constexpr auto k_size = 32;
    auto const extent = tg::vec2i(k_size, k_size);

    // Compiled first, and awaited on the assets themselves: a shader builds on the library's own scheduler rather
    // than on the context's backlog, so polling the latter would wait forever.
    // A member does this through its routine's init; a test has no routine, so it does it here.
    for (auto const& asset : {sr::shaders::nn_conv.compute.main_cs, sr::shaders::nn_input.compute.main_cs,
                              sr::shaders::nn_output.compute.main_cs, sr::shaders::nn_pool.compute.main_cs,
                              sr::shaders::nn_upsample.compute.main_cs})
    {
        auto const shader = asset->acquire(ctx);
        co_await cc::async_settled(shader);
        if (shader->has_error())
            FAIL(cc::format("a network shader did not compile: {}", shader->try_error()->underlying().to_string()));
    }

    // A refusal is logged with its reason, and the commonest is simply that nobody fetched the weights.
    // Said as "could not be created" rather than naming one cause, because it has several and a message that picks
    // the wrong one sends the next reader somewhere else entirely.
    auto network = sr::impl::oidn_network();
    if (!network.create(ctx, extent))
        SKIP("the network could not be created; the commonest reason is unfetched weights "
             "(extern/oidn-weights/fetch-oidn-weights.py), and sr's log says which it was");

    // The pipelines are built from the compiled shaders, which may still be a tick behind.
    auto ready = network.prepare();
    for (auto attempt = 0; attempt < 16 && !ready; ++attempt)
    {
        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
        ready = network.prepare();
    }
    REQUIRE(ready).context("the network's pipelines never finished building");

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const color = make(sg::pixel_format::rgba32_float);
    auto const albedo = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const output = make(sg::pixel_format::rgba32_float);

    // A lit checkerboard with noise on it, which is the kind of image the network was trained to see.
    auto color_pixels = cc::vector<tg::vec4f>();
    auto albedo_pixels = cc::vector<tg::vec4f>();
    auto normal_pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const bright = ((x / 8 + y / 8) % 2) == 0;
            auto const a = bright ? tg::vec4f(0.8f, 0.6f, 0.3f, 0) : tg::vec4f(0.1f, 0.2f, 0.5f, 0);
            albedo_pixels.push_back(a);

            // Deterministic speckle, so the network has something to remove.
            auto const n = f32(((x * 7 + y * 13) % 11)) / 11.0f;
            color_pixels.push_back(tg::vec4f(a[0] * (0.4f + n), a[1] * (0.4f + n), a[2] * (0.4f + n), 0));

            normal_pixels.push_back(tg::vec4f(0, 0, 1, 0));
        }

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(color_pixels).as_bytes());
    cmd->upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(albedo_pixels).as_bytes());
    cmd->upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(normal_pixels).as_bytes());

    REQUIRE(network.execute(*cmd, color, albedo, normal, output, 1.0f));

    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_size * k_size);

    // Finite and non-negative everywhere.
    //
    // A single mismatched stride anywhere in sixteen layers reaches every pixel through the pooling, so this is a
    // weaker check than it looks only if the network is right — if it is wrong, it is usually NaN everywhere.
    auto finite = 0;
    auto negative = 0;
    for (auto const& p : got)
        for (auto c = 0; c < 3; ++c)
        {
            if (p[c] == p[c] && tg::abs(p[c]) < 1e30f)
                ++finite;
            if (p[c] < 0.0f)
                ++negative;
        }
    CHECK(finite == k_size * k_size * 3).context(cc::format("{} of {} channels are finite", finite, k_size * k_size * 3));
    CHECK(negative == 0).context(cc::format("{} channels are negative, which the output ReLU forbids", negative));

    // The result is an image rather than a constant.
    //
    // A network whose weights never reached the shader produces the bias pattern alone, which is uniform — so this is
    // what separates "the weights were uploaded" from "a plausible-looking flat field".
    auto lowest = got[0][0];
    auto highest = got[0][0];
    for (auto const& p : got)
        for (auto c = 0; c < 3; ++c)
        {
            lowest = cc::min(lowest, p[c]);
            highest = cc::max(highest, p[c]);
        }
    CHECK(highest - lowest > 1e-3f)
        .context(cc::format("the output spans {} to {}, which is flat enough to be the bias alone", lowest, highest));
}
