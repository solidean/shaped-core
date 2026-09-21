#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/oidn_device.hh>
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

// The one test that says whether any of this is RIGHT.
//
// Everything else checks a piece against its own definition.
// The convolution against a reference loop, the transfer against its own inverse, the graph against the widths in the
// weights file.
// All of those can hold while the whole still computes something Intel's implementation does not: a transposed weight
// index that is self-consistent, a transfer curve wrong in a way the round trip cancels, a different padding.
//
// So this runs OIDN's own filter over the same input and compares, which is the only way to ask the question.
ASYNC_INVOCABLE_TEST("sr - the network agrees with OIDN's own filter",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    if (!sr::impl::oidn_is_compiled_in() || !sr::impl::oidn_has_device())
        SKIP("OIDN itself was not fetched, so there is nothing to compare against");

    auto lib = slib::shader_library();
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the network's shaders");
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());

    for (auto const& asset : {sr::shaders::nn_conv.compute.main_cs, sr::shaders::nn_input.compute.main_cs,
                              sr::shaders::nn_output.compute.main_cs, sr::shaders::nn_pool.compute.main_cs,
                              sr::shaders::nn_upsample.compute.main_cs})
    {
        auto const shader = asset->acquire(ctx);
        co_await cc::async_settled(shader);
        if (shader->has_error())
            FAIL(cc::format("a network shader did not compile: {}", shader->try_error()->underlying().to_string()));
    }

    constexpr auto k_size = 64;
    auto const extent = tg::vec2i(k_size, k_size);

    // A lit, textured surface with speckle on it — the kind of image the weights were trained over, rather than a
    // pattern chosen to be easy.
    auto color3 = cc::vector<tg::vec3f>();
    auto albedo3 = cc::vector<tg::vec3f>();
    auto normal3 = cc::vector<tg::vec3f>();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const bright = ((x / 16 + y / 16) % 2) == 0;
            auto const a = bright ? tg::vec3f(0.8f, 0.6f, 0.3f) : tg::vec3f(0.1f, 0.2f, 0.5f);
            auto const speckle = 0.35f + f32((x * 7 + y * 13) % 11) / 11.0f;
            albedo3.push_back(a);
            color3.push_back(a * speckle);
            normal3.push_back(tg::vec3f(0, 0, 1));
        }

    auto reference = cc::vector<tg::vec3f>::create_filled(size_t(k_size * k_size), tg::vec3f(0, 0, 0));
    REQUIRE(sr::impl::oidn_filter_reference(color3, albedo3, normal3, extent, reference));

    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const color = make();
    auto const albedo = make();
    auto const normal = make();
    auto const output = make();

    auto const to_rgba = [](cc::span<tg::vec3f const> v)
    {
        auto out = cc::vector<tg::vec4f>();
        out.reserve(v.size());
        for (auto const& p : v)
            out.push_back(tg::vec4f(p[0], p[1], p[2], 0));
        return out;
    };

    auto network = sr::impl::oidn_network();
    if (!network.create(ctx, extent))
        SKIP("the network could not be created; sr's log says why");

    auto ready = network.prepare();
    for (auto attempt = 0; attempt < 16 && !ready; ++attempt)
    {
        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
        ready = network.prepare();
    }
    REQUIRE(ready).context("the network's pipelines never finished building");

    auto cmd = ctx.create_command_list();
    auto const color4 = to_rgba(color3);
    auto const albedo4 = to_rgba(albedo3);
    auto const normal4 = to_rgba(normal3);
    cmd->upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(color4).as_bytes());
    cmd->upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(albedo4).as_bytes());
    cmd->upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(normal4).as_bytes());

    REQUIRE(network.execute(*cmd, color, albedo, normal, output, 1.0f));

    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_size * k_size);

    // Reported as the worst and the mean over the interior, because the two say different things: a wrong constant
    // moves the mean, and a wrong index usually moves one region a lot while leaving the rest alone.
    auto worst = 0.0f;
    auto worst_at = tg::vec2i(0, 0);
    auto total = 0.0;
    auto samples = 0;
    for (auto y = 4; y < k_size - 4; ++y)
        for (auto x = 4; x < k_size - 4; ++x)
            for (auto c = 0; c < 3; ++c)
            {
                auto const mine = got[y * k_size + x][c];
                auto const theirs = reference[y * k_size + x][c];
                auto const d = tg::abs(mine - theirs);
                total += d;
                ++samples;
                if (d > worst)
                {
                    worst = d;
                    worst_at = tg::vec2i(x, y);
                }
            }

    // The bounds have roughly a decade of headroom over what this machine actually returns — a mean of 1.0e-05 and a
    // worst of 7.5e-05, which is what sixteen layers of fp32 on the GPU against OIDN's own CPU inference costs.
    // Loose enough not to chase a driver, and orders of magnitude tighter than any real mistake: a transposed weight
    // index or a wrong padding moves whole regions, not the fifth significant figure.
    auto const mean = f32(total / f64(samples));
    CHECK(mean < 1e-4f).context(cc::format("mean difference {} over {} samples", mean, samples));
    CHECK(worst < 1e-3f)
        .context(cc::format("worst difference {} at {},{} (ours {}, OIDN {})", worst, worst_at[0], worst_at[1],
                            got[worst_at[1] * k_size + worst_at[0]][0], reference[worst_at[1] * k_size + worst_at[0]][0]));

    // And the comparison was worth making: OIDN's own output has to differ from what went in, or "we agree" would
    // only be saying that neither of us did anything.
    auto changed = 0.0;
    for (auto n = 0; n < k_size * k_size; ++n)
        for (auto c = 0; c < 3; ++c)
            changed += f64(tg::abs(reference[n][c] - color3[n][c]));
    CHECK(changed / f64(k_size * k_size * 3) > 0.01)
        .context(cc::format("OIDN changed the image by {} per channel, which is close enough to nothing that agreeing "
                            "with it says nothing",
                            changed / f64(k_size * k_size * 3)));
}
