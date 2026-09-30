#include "oidn_reference.hh"
#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/oidn_denoise_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

namespace
{
/// An image for the oracles: what OIDN is compared against has to reach every part of the input it prepares.
struct oracle_scene
{
    cc::vector<tg::vec3f> color;
    cc::vector<tg::vec3f> albedo;
    cc::vector<tg::vec3f> normal;
};

/// A textured, bumpy surface whose radiance spans ten decades.
///
/// Radiance runs log-linearly from 1e-7 at one corner to 1e3 at the other, so it crosses the transfer curve's linear
/// toe, its power segment and its log tail, where a narrow range would test only one.
/// Each 16-pixel cell is a hemisphere bump, so a normal's x and y take both signs: a swapped or negated component
/// in the input packing then changes the image rather than hiding under a constant `(0, 0, 1)`.
[[nodiscard]] oracle_scene make_oracle_scene(int size)
{
    auto scene = oracle_scene();
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const bright = ((x / 16 + y / 16) % 2) == 0;
            auto const a = bright ? tg::vec3f(0.8f, 0.6f, 0.3f) : tg::vec3f(0.1f, 0.2f, 0.5f);

            auto const u = (f32(x % 16) + 0.5f) / 8.0f - 1.0f;
            auto const v = (f32(y % 16) + 0.5f) / 8.0f - 1.0f;
            auto const r2 = u * u + v * v;
            auto const n = r2 < 1.0f ? tg::vec3f(u, v, tg::sqrt(1.0f - r2)) : tg::vec3f(0, 0, 1);

            auto const t = f32(x + y) / f32(cc::max(2 * size - 2, 1));
            auto const radiance = tg::pow(10.0f, -7.0f + 10.0f * t);
            auto const speckle = 0.35f + f32((x * 7 + y * 13) % 11) / 11.0f;

            scene.albedo.push_back(a);
            scene.normal.push_back(n);
            scene.color.push_back(a * (speckle * radiance));
        }
    return scene;
}

/// How far `got` is from `reference`, relative to the reference, over every pixel.
///
/// Relative because the scene spans ten decades, where an absolute difference would weigh only the brightest corner;
/// floored at 1e-3 so a value near zero does not turn rounding into an error.
struct oracle_error
{
    f64 mean = 0.0;
    f64 worst = 0.0;
    tg::vec2i worst_at = tg::vec2i(0, 0);
};

/// The oracles' bounds on the relative difference, about a decade above what this machine returns.
///
/// Measured over the scene above, every pixel: a mean of 8.8e-07 and a worst of 1.2e-05 for the base network,
/// 1.1e-06 and 1.2e-05 for the small one, and 1.0e-06 and 1.6e-05 for the small one tiled 2x2.
/// Two mistakes they have to see, measured: padding the tensor by repeating the image's edge instead of with zeros
/// moves the mean to 2.5e-02 and the worst to 0.78, and decoding subnormal fp16 weights one exponent too high moves
/// them to 4.4e-05 and 6.7e-04.
constexpr f64 k_oracle_mean = 1e-5;
constexpr f64 k_oracle_worst = 2e-4;

[[nodiscard]] oracle_error compare_to_reference(cc::span<tg::vec4f const> got, cc::span<tg::vec3f const> reference, int size)
{
    auto error = oracle_error();
    auto total = 0.0;
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            for (auto c = 0; c < 3; ++c)
            {
                auto const theirs = f64(reference[y * size + x][c]);
                auto const d = tg::abs(f64(got[y * size + x][c]) - theirs) / cc::max(tg::abs(theirs), 1e-3);
                total += d;
                if (d > error.worst)
                {
                    error.worst = d;
                    error.worst_at = tg::vec2i(x, y);
                }
            }
    error.mean = total / f64(size * size * 3);
    return error;
}
} // namespace

// The whole U-Net: nine packed channels in, sixteen convolutions, four pools, four upsamples, three channels out.
//
// What this can check on its own is that the graph HOLDS TOGETHER — every layer finds weights of the width its
// source produces, every skip concatenation adds up, and the result is finite radiance rather than a NaN that one
// mismatched stride would spread across the whole image.
// Whether it computes what Intel's implementation computes is a different question, and the answer to it is a
// comparison against OIDN's own filter rather than anything assertable here.

ASYNC_INVOCABLE_TEST("sr - the denoise network runs end to end", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // Deliberately NOT a multiple of sixteen, and not square.
    // Four pools need one, so the network pads its tensors up with zeros — an arbitrary view size is the normal case,
    // and an aligned one would never exercise that.
    constexpr auto k_width = 40;
    constexpr auto k_height = 24;
    auto const extent = tg::vec2i(k_width, k_height);

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

    // Unfetched weights are the one reason to skip; with them present, a network that cannot be created is a failure,
    // since that is exactly what a broken weights bump or loader change looks like.
    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");
    auto network = sr::impl::oidn_network();
    REQUIRE(network.create(ctx, extent)).context("the weights are present and still did not load; sr's log says why");

    // The pipelines are built from the compiled shaders, which may still be a tick behind.
    auto ready = network.prepare();
    for (auto attempt = 0; attempt < 16 && !ready; ++attempt)
    {
        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
        ready = network.prepare();
    }
    REQUIRE(ready).context("the network's pipelines never finished building");

    CHECK(network.extent() == extent);
    CHECK(network.padded_extent() == tg::vec2i(48, 32))
        .context(cc::format("padded to {}x{}", network.padded_extent()[0], network.padded_extent()[1]));

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_width,
                                                 .height = k_height,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
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
    for (auto y = 0; y < k_height; ++y)
        for (auto x = 0; x < k_width; ++x)
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
    auto const raw_readback = sg::data_future<f32>(cmd->download.data_from_buffer(network.output_tensor()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_width * k_height);

    // Finite everywhere, read from the network's last tensor rather than from the image.
    //
    // A single mismatched stride anywhere in sixteen layers reaches every pixel through the pooling, and when it is
    // wrong it is usually NaN everywhere.
    // The output pass turns a NaN into black, so only the tensor before it can show one.
    auto const raw = co_await raw_readback.data();
    REQUIRE(raw.size() == network.padded_extent()[0] * network.padded_extent()[1] * 4);
    auto finite = 0;
    for (auto const v : raw)
        if (v == v && tg::abs(v) < 1e30f)
            ++finite;
    CHECK(finite == raw.size()).context(cc::format("{} of {} values in the last tensor are finite", finite, raw.size()));

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
ASYNC_INVOCABLE_TEST("sr - the network agrees with OIDN's own filter", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    if (!sr_test::oidn_is_compiled_in() || !sr_test::oidn_has_device())
        SKIP("OIDN itself was not fetched, so there is nothing to compare against "
             "(uv run extern/oidn/fetch-oidn.py)");
    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    for (auto const& asset : {sr::shaders::nn_conv.compute.main_cs, sr::shaders::nn_input.compute.main_cs,
                              sr::shaders::nn_output.compute.main_cs, sr::shaders::nn_pool.compute.main_cs,
                              sr::shaders::nn_upsample.compute.main_cs})
    {
        auto const shader = asset->acquire(ctx);
        co_await cc::async_settled(shader);
        if (shader->has_error())
            FAIL(cc::format("a network shader did not compile: {}", shader->try_error()->underlying().to_string()));
    }

    // Not a multiple of sixteen, so the right and bottom edges are next to padding, and every pixel is compared.
    constexpr auto k_size = 72;
    auto const extent = tg::vec2i(k_size, k_size);

    auto const scene = make_oracle_scene(k_size);
    auto const& color3 = scene.color;
    auto const& albedo3 = scene.albedo;
    auto const& normal3 = scene.normal;

    // Both networks, each against the quality whose weights it is: the small one is OIDN's `fast`, the base one its
    // `balanced`.
    for (auto const size : {sr::oidn_network_size::base, sr::oidn_network_size::small})
    {
        auto const label = size == sr::oidn_network_size::small ? "small" : "base";

        auto reference = cc::vector<tg::vec3f>::create_filled(size_t(k_size * k_size), tg::vec3f(0, 0, 0));
        REQUIRE(sr_test::oidn_filter_reference(color3, albedo3, normal3, extent, reference, size));

        auto const make = [&]
        {
            return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                     .width = k_size,
                                                     .height = k_size,
                                                     .usage = sg::texture_usage::texture | sg::texture_usage::image
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
        REQUIRE(network.create(ctx, extent, sr::impl::oidn_network::k_default_tile,
                               sr::impl::oidn_network::k_tile_overlap, size))
            .context(cc::format("{}: the weights are present and still did not load; sr's log says why", label));

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

        // Reported as the worst and the mean, because the two say different things: a wrong constant moves the mean,
        // and a wrong index or a wrong padding usually moves one region a lot while leaving the rest alone.
        auto const e = compare_to_reference(got, reference, k_size);
        auto const at = e.worst_at[1] * k_size + e.worst_at[0];
        CHECK(e.mean < k_oracle_mean).context(cc::format("{}: mean relative difference {}", label, e.mean));
        CHECK(e.worst < k_oracle_worst)
            .context(cc::format("{}: worst relative difference {} at {},{} (ours {}, OIDN {})", label, e.worst,
                                e.worst_at[0], e.worst_at[1], got[at][0], reference[at][0]));

        // And the comparison was worth making: OIDN's own output has to differ from what went in, or "we agree" would
        // only be saying that neither of us did anything.
        auto changed = 0.0;
        for (auto n = 0; n < k_size * k_size; ++n)
            for (auto c = 0; c < 3; ++c)
                changed += tg::abs(f64(reference[n][c]) - f64(color3[n][c])) / cc::max(f64(color3[n][c]), 1e-3);
        CHECK(changed / f64(k_size * k_size * 3) > 0.01)
            .context(cc::format("OIDN changed the image by {} per channel, which is close enough to nothing that "
                                "agreeing "
                                "with it says nothing",
                                changed / f64(k_size * k_size * 3)));
    }
}

// The member, through the framework rather than through the network directly.
//
// What this adds over the tests above is everything between `sr::reconstruct_routine` and the shaders: that the method
// resolves, that the guide contract is enforced, that the network lands in the caller's history and is reused, and
// that a second call on the same history does not rebuild it.
ASYNC_INVOCABLE_TEST("sr - the OIDN member denoises through the denoise front", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    if (!sr::query_reconstruct_support(ctx).oidn)
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    // A named member resolves to itself; `automatic` never picks it, being far too slow for a frame loop.
    auto const settings = sr::reconstruct_settings{.denoiser = sr::denoise_method::oidn};
    CHECK(sr::resolve_denoise_method(ctx, settings) == sr::denoise_method::oidn);
    CHECK(!sr::is_temporal(sr::denoise_method::oidn));
    CHECK(sr::resolve_denoise_method(ctx, {.denoiser = sr::denoise_method::automatic}) != sr::denoise_method::oidn);
    CHECK(sr::resolve_denoise_method(ctx, {.denoiser = sr::denoise_method::automatic, .fresh_samples = true})
          != sr::denoise_method::oidn);

    // `quality` picks the network the way OIDN's own setting does.
    CHECK(sr::oidn_denoise_routine::options_for({.quality = sr::denoise_quality::fast}).network
          == sr::oidn_network_size::small);
    CHECK(sr::oidn_denoise_routine::options_for({.quality = sr::denoise_quality::balanced}).network
          == sr::oidn_network_size::base);
    CHECK(sr::oidn_denoise_routine::options_for({.quality = sr::denoise_quality::best}).network
          == sr::oidn_network_size::base);

    // The member's `init` and the network's five pipelines, both settled before the first call.
    // The pipelines compile on slib's queue, which nothing an epoch advance drains, so they are awaited directly.
    sr::oidn_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();
    REQUIRE(co_await sr::impl::oidn_prewarm_pipelines(ctx)).context("the network's pipelines did not build");

    constexpr auto k_width = 48;
    constexpr auto k_height = 48;

    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = k_width,
                                                 .height = k_height,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const color = make();
    auto const albedo = make();
    auto const normal = make();
    auto const output = make();

    auto color_pixels = cc::vector<tg::vec4f>();
    auto albedo_pixels = cc::vector<tg::vec4f>();
    auto normal_pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_height; ++y)
        for (auto x = 0; x < k_width; ++x)
        {
            auto const a = ((x / 12 + y / 12) % 2) == 0 ? tg::vec4f(0.8f, 0.6f, 0.3f, 0) : tg::vec4f(0.1f, 0.2f, 0.5f, 0);
            auto const speckle = 0.35f + f32((x * 7 + y * 13) % 11) / 11.0f;
            albedo_pixels.push_back(a);
            color_pixels.push_back(tg::vec4f(a[0] * speckle, a[1] * speckle, a[2] * speckle, 0));
            normal_pixels.push_back(tg::vec4f(0, 0, 1, 0));
        }

    auto history = sr::reconstruct_history();

    auto const run = [&](sg::command_list& cmd)
    {
        cmd.upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(color_pixels).as_bytes());
        cmd.upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(albedo_pixels).as_bytes());
        cmd.upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(normal_pixels).as_bytes());

        auto const in = sr::reconstruct_inputs{
            .color = color,
            .guides = {.albedo = albedo, .normal = normal},
            .output = output,
        };
        return sr::reconstruct_routine::execute(cmd, in, history, settings);
    };

    // With everything built, the very first call denoises: a member that answered `pending` here would leave a real
    // frame loop showing the raw image for as long as it kept answering so.
    auto cmd = ctx.create_command_list();
    auto const outcome = run(*cmd);
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    REQUIRE(outcome.is_denoised()).context(cc::format("the first call's status was {}", int(outcome.status)));
    auto const first_restarted = outcome.restarted;
    CHECK(outcome.denoiser == sr::denoise_method::oidn);
    CHECK(first_restarted).context("the first call on a fresh history starts from nothing");
    CHECK(history.denoiser() == sr::denoise_method::oidn);
    CHECK(history.extent() == tg::vec2i(k_width, k_height));

    // A second call reuses what the first built, which is the whole reason the network lives in the history.
    auto cmd2 = ctx.create_command_list();
    auto const second = run(*cmd2);
    auto const readback = sg::data_future<tg::vec4f>(cmd2->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd2));
    ctx.advance_epoch();

    CHECK(second.is_denoised());
    CHECK(!second.restarted).context("the second call rebuilt the network, which it should have reused");

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_width * k_height);

    // Denoised rather than merely written: the speckle is gone where the albedo is flat.
    //
    // Measured as the difference between neighbours inside one checker cell, which noise inflates and a denoiser
    // brings down — against the same measure on the input.
    auto const roughness = [&](auto const& image)
    {
        auto sum = 0.0;
        auto count = 0;
        for (auto y = 14; y < 22; ++y)
            for (auto x = 14; x < 22; ++x)
                for (auto c = 0; c < 3; ++c)
                {
                    sum += f64(tg::abs(image[y * k_width + x][c] - image[y * k_width + x + 1][c]));
                    ++count;
                }
        return sum / f64(count);
    };

    auto const before = roughness(color_pixels);
    auto const after = roughness(got);
    CHECK(after < 0.5 * before).context(cc::format("roughness went from {} to {}", before, after));

    // Smoothness alone would also be satisfied by a blank image, which is the failure a member that writes nothing
    // produces — so the mean has to survive the round trip too.
    auto const mean = [&](auto const& image)
    {
        auto sum = 0.0;
        for (auto y = 8; y < k_height - 8; ++y)
            for (auto x = 8; x < k_width - 8; ++x)
                for (auto c = 0; c < 3; ++c)
                    sum += f64(image[y * k_width + x][c]);
        return sum / f64((k_height - 16) * (k_width - 16) * 3);
    };

    auto const mean_in = mean(color_pixels);
    auto const mean_out = mean(got);
    CHECK(mean_out > 0.7 * mean_in);
    CHECK(mean_out < 1.4 * mean_in).context(cc::format("mean went from {} to {}", mean_in, mean_out));
}

// What the network costs at a real resolution when run whole, which is why it runs in tiles.
//
// Asked of a small network rather than a large one: the widths come from the weights and the extents are arithmetic,
// so the figure for 1080p is computable without allocating a byte of it.
ASYNC_INVOCABLE_TEST("sr - the OIDN network run whole needs gigabytes, which is why it tiles",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // `create` starts the pipelines compiling, and this test asks a question that never runs them.
    // Drained rather than abandoned, because work still carrying a finished test's context is what nexus reports.
    (void)co_await sr::impl::oidn_prewarm_pipelines(ctx);

    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");
    auto network = sr::impl::oidn_network();
    REQUIRE(network.create(ctx, tg::vec2i(64, 64))).context("the weights are present and still did not load");

    auto const mib = [](i64 bytes) { return f64(bytes) / (1024.0 * 1024.0); };

    // The twenty-five feature maps are the whole cost; the weights are a few megabytes beside them.
    auto const at_64 = network.feature_bytes_for(tg::vec2i(64, 64));
    auto const at_1080p = network.feature_bytes_for(tg::vec2i(1920, 1080));
    auto const at_4k = network.feature_bytes_for(tg::vec2i(3840, 2160));

    CHECK(at_64 == network.feature_bytes());

    // Quadratic in the image, because every map is a fraction of it.
    // 1080p is 510x the pixels of 64x64, and the cost tracks that within the padding's slack.
    CHECK(f64(at_1080p) / f64(at_64) > 400.0);
    CHECK(f64(at_1080p) / f64(at_64) < 600.0)
        .context(cc::format("64x64 {} MiB, 1080p {} MiB, 4K {} MiB", mib(at_64), mib(at_1080p), mib(at_4k)));

    // The number this test exists to pin: one 1080p frame run whole wants over 2 GiB of feature maps.
    // That is what makes tiling a prerequisite for a real image rather than an optimisation.
    CHECK(at_1080p > i64(2) * 1024 * 1024 * 1024).context(cc::format("1080p needs {} MiB of feature maps", mib(at_1080p)));

    co_return;
}

// Tiles against the whole image, which is the only thing that can price the overlap.
//
// The network is the same either way; what tiling changes is what each pixel could see while it was computed.
// So the question is entirely whether the overlap is wide enough and every tile sits on the network's grid, and the
// answer is a comparison rather than an argument.
//
// Three shapes, because each breaks a different part of the placement.
// A square that is a multiple of sixteen is the plain case.
// An image that is NOT a multiple of sixteen puts the last tile's origin where only an aligned clamp keeps it on the
// 16-pixel grid four pools need, and an origin off that grid pools over different windows than the whole run does.
// A wide, short image overflows the cap on one axis only, and the other must then stay a single untiled row.
ASYNC_INVOCABLE_TEST("sr - the OIDN network in tiles agrees with the same image run whole",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    REQUIRE(co_await sr::impl::oidn_prewarm_pipelines(ctx)).context("the network's pipelines did not build");

    struct tiling_case
    {
        tg::vec2i extent = tg::vec2i(0, 0);
        int cap = 0;
        tg::vec2i tiles = tg::vec2i(0, 0); // what `cap` must choose
        sr::oidn_network_size size = sr::oidn_network_size::base;
    };

    // The default case is the cheapest shape that still reaches both the 16-pixel grid and the zero padding.
    // 344 is not a multiple of sixteen, and a 336 cap tiles it 2x2 with the last tile shifted inward to origin 16,
    // where clamping to the image instead would put it at 8.
    // It runs the small network, since what is checked is where tiles sit rather than which weights run.
    // Every GPU test here runs on WARP in CI, where each has to finish inside the watchdog's 90 s; the larger shapes
    // run in a thorough pass.
    auto cases = cc::vector<tiling_case>();
    cases.push_back(
        {.extent = tg::vec2i(344, 344), .cap = 336, .tiles = tg::vec2i(2, 2), .size = sr::oidn_network_size::small});
    if (nx::is_thorough())
    {
        cases.push_back({.extent = tg::vec2i(400, 392), .cap = 288, .tiles = tg::vec2i(4, 4)});
        cases.push_back({.extent = tg::vec2i(384, 384), .cap = 288, .tiles = tg::vec2i(3, 3)});
        cases.push_back({.extent = tg::vec2i(608, 200), .cap = 288, .tiles = tg::vec2i(5, 1)});
    }

    for (auto const& tc : cases)
    {
        auto const w = tc.extent[0];
        auto const h = tc.extent[1];
        auto const label = cc::format("{}x{}", w, h);

        auto const make = [&]
        {
            return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                     .width = w,
                                                     .height = h,
                                                     .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                            | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
        };

        auto const color = make();
        auto const albedo = make();
        auto const normal = make();
        auto const whole_output = make();
        auto const tiled_output = make();

        // Structure at several scales, so a tile seam would have something to break.
        // A flat image would hide an insufficient overlap completely.
        auto color_pixels = cc::vector<tg::vec4f>();
        auto albedo_pixels = cc::vector<tg::vec4f>();
        auto normal_pixels = cc::vector<tg::vec4f>();
        for (auto y = 0; y < h; ++y)
            for (auto x = 0; x < w; ++x)
            {
                auto const coarse = ((x / 24 + y / 24) % 2) == 0;
                auto const fine = ((x / 3 + y / 5) % 2) == 0;
                auto const a = coarse ? tg::vec4f(0.8f, 0.6f, 0.3f, 0) : tg::vec4f(0.1f, 0.2f, 0.5f, 0);
                auto const gradient = f32(x + y) / f32(w + h);
                auto const speckle = 0.3f + f32((x * 7 + y * 13) % 11) / 11.0f + (fine ? 0.2f : 0.0f);

                albedo_pixels.push_back(a);
                color_pixels.push_back(tg::vec4f(a[0] * speckle * (0.5f + gradient), a[1] * speckle,
                                                 a[2] * speckle * (1.5f - gradient), 0));
                normal_pixels.push_back(tg::vec4f(0, 0, 1, 0));
            }

        // The whole run takes a cap no axis exceeds; the tiled one takes the case's.
        auto const overlap = sr::impl::oidn_network::k_tile_overlap;
        auto whole = sr::impl::oidn_network();
        REQUIRE(whole.create(ctx, tc.extent, 1024, overlap, tc.size));
        CHECK(whole.tile_counts() == tg::vec2i(1, 1)).context(label);
        CHECK(whole.tile_overlap() == 0).context(label);

        auto tiled = sr::impl::oidn_network();
        REQUIRE(tiled.create(ctx, tc.extent, tc.cap, overlap, tc.size));
        CHECK(tiled.tile_counts() == tc.tiles)
            .context(cc::format("{} chose {}x{} tiles", label, tiled.tile_counts()[0], tiled.tile_counts()[1]));
        CHECK(tiled.tile_overlap() == sr::impl::oidn_network::k_tile_overlap).context(label);

        // The whole point of tiling, restated as a number: the tensors are sized by the tile, not by the image.
        CHECK(tiled.feature_bytes() < whole.feature_bytes())
            .context(cc::format("{}: tiled {} bytes vs whole {} bytes", label, tiled.feature_bytes(),
                                whole.feature_bytes()));

        REQUIRE(whole.prepare());
        REQUIRE(tiled.prepare());

        auto cmd = ctx.create_command_list();
        cmd->upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(color_pixels).as_bytes());
        cmd->upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(albedo_pixels).as_bytes());
        cmd->upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(normal_pixels).as_bytes());

        REQUIRE(whole.execute(*cmd, color, albedo, normal, whole_output, 1.0f));
        REQUIRE(tiled.execute(*cmd, color, albedo, normal, tiled_output, 1.0f));

        auto const whole_read = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(whole_output.raw()));
        auto const tiled_read = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(tiled_output.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        auto const a = co_await whole_read.data();
        auto const b = co_await tiled_read.data();
        REQUIRE(a.size() == w * h);
        REQUIRE(b.size() == w * h);

        // Every pixel is written exactly once, so a gap between tiles shows up as an untouched texel rather than a seam.
        auto unwritten = 0;
        for (auto i = 0; i < b.size(); ++i)
            if (b[i][3] != 1.0f)
                ++unwritten;
        CHECK(unwritten == 0).context(cc::format("{}: {} texels no tile wrote", label, unwritten));

        auto worst = 0.0;
        auto sum = 0.0;
        auto worst_at = tg::vec2i(0, 0);
        for (auto y = 0; y < h; ++y)
            for (auto x = 0; x < w; ++x)
                for (auto c = 0; c < 3; ++c)
                {
                    auto const d = f64(tg::abs(a[y * w + x][c] - b[y * w + x][c]));
                    sum += d;
                    if (d > worst)
                    {
                        worst = d;
                        worst_at = tg::vec2i(x, y);
                    }
                }

        auto const mean = sum / f64(w * h * 3);

        // The bound the overlap is chosen against.
        // An overlap too narrow, or a tile off the pool grid, shows up here and nowhere else, and at a tile boundary
        // rather than spread out.
        // For scale: the square is 2.8e-01 out with no overlap, and 1.4e-03 out at an overlap of 64.
        CHECK(mean < 1.0e-6).context(cc::format("{}: mean difference {}", label, mean));
        CHECK(worst < 1.0e-4)
            .context(
                cc::format("{}: worst difference {} at {},{} (mean {})", label, worst, worst_at[0], worst_at[1], mean));
    }

    co_return;
}

// The TILED path against OIDN's own filter, which is the comparison that covers what a real image runs through.
//
// The oracle above runs at 64x64 and fits one tile, so it never exercises tiling at all.
// The tiling test beside it compares against our own whole-image run rather than against Intel.
// This closes that: OIDN filters the whole image, we filter it in nine tiles, and the two are put side by side.
ASYNC_INVOCABLE_TEST("sr - the tiled network agrees with OIDN's own filter", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    if (!sr_test::oidn_is_compiled_in() || !sr_test::oidn_has_device())
        SKIP("OIDN itself was not fetched, so there is nothing to compare against "
             "(uv run extern/oidn/fetch-oidn.py)");
    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    REQUIRE(co_await sr::impl::oidn_prewarm_pipelines(ctx)).context("the network's pipelines did not build");

    // Not a multiple of sixteen either, so the last tile row and column sit next to the padding.
    // The small network against Intel's fast quality, at 2x2 tiles, so it fits WARP's budget in CI; the untiled
    // oracle holds both networks to Intel already.
    constexpr auto k_size = 344;
    constexpr auto k_network = sr::oidn_network_size::small;
    auto const extent = tg::vec2i(k_size, k_size);

    auto const scene = make_oracle_scene(k_size);
    auto const& color3 = scene.color;
    auto const& albedo3 = scene.albedo;
    auto const& normal3 = scene.normal;

    auto reference = cc::vector<tg::vec3f>::create_filled(size_t(k_size * k_size), tg::vec3f(0, 0, 0));
    REQUIRE(sr_test::oidn_filter_reference(color3, albedo3, normal3, extent, reference, k_network));

    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
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

    // A 336 cap forces a genuinely tiled run: its interior is the 176 left after 80 on each side, so a 344 image
    // takes two tiles per axis.
    auto network = sr::impl::oidn_network();
    REQUIRE(network.create(ctx, extent, 336, sr::impl::oidn_network::k_tile_overlap, k_network));
    REQUIRE(network.tile_counts() == tg::vec2i(2, 2))
        .context(cc::format("tiled {}x{}", network.tile_counts()[0], network.tile_counts()[1]));
    REQUIRE(network.prepare());

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

    // The same bounds the untiled oracle carries: tiling has to cost nothing measurable in agreement with Intel.
    auto const e = compare_to_reference(got, reference, k_size);
    CHECK(e.mean < k_oracle_mean).context(cc::format("mean relative difference {}", e.mean));
    CHECK(e.worst < k_oracle_worst)
        .context(cc::format("worst relative difference {} at {},{} (mean {})", e.worst, e.worst_at[0], e.worst_at[1],
                            e.mean));
}
