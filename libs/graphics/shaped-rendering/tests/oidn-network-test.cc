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
#include <shaped-rendering/oidn_denoise_routine.hh>
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

    // Deliberately NOT a multiple of sixteen, and not square.
    // Four pools need one, so the network pads its tensors up and repeats the image's edge into the padding — an
    // arbitrary view size is the normal case, and an aligned one would never exercise that.
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

    CHECK(network.extent() == extent);
    CHECK(network.padded_extent() == tg::vec2i(48, 32))
        .context(cc::format("padded to {}x{}", network.padded_extent()[0], network.padded_extent()[1]));

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_width,
                                                 .height = k_height,
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
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_width * k_height);

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
    CHECK(finite == k_width * k_height * 3)
        .context(cc::format("{} of {} channels are finite", finite, k_width * k_height * 3));
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

// The member, through the framework rather than through the network directly.
//
// What this adds over the tests above is everything between `sr::denoise_routine` and the shaders: that the method
// resolves, that the guide contract is enforced, that the network lands in the caller's history and is reused, and
// that a second call on the same history does not rebuild it.
ASYNC_INVOCABLE_TEST("sr - the OIDN member denoises through the denoise front",
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

    if (!sr::query_denoise_support(ctx).oidn)
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    // A named member resolves to itself, and it is spatial — so it is what `automatic` reaches for on a converging
    // mean, ahead of a-trous.
    auto const settings = sr::denoise_settings{.method = sr::denoise_method::oidn};
    CHECK(sr::resolve_denoise_method(ctx, settings, false) == sr::denoise_method::oidn);
    CHECK(!sr::is_temporal(sr::denoise_method::oidn));
    CHECK(sr::resolve_denoise_method(ctx, {.method = sr::denoise_method::automatic}, false) == sr::denoise_method::oidn)
        .context("automatic should prefer the trained spatial member over a-trous");

    sr::oidn_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    // The same wait the member's `init` performs, repeated here against THIS test's shader library.
    // The routine initializes once per context, and every test in this binary brings its own library, so by the time
    // this one runs the pipelines `init` built belong to a library that is gone — and the first call would otherwise
    // sit on a compile that nothing in a frame loop drives.
    REQUIRE(co_await sr::impl::oidn_prewarm_pipelines(ctx)).context("the network's pipelines did not build");

    constexpr auto k_width = 48;
    constexpr auto k_height = 48;

    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = k_width,
                                                 .height = k_height,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
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

    auto history = sr::denoise_history();

    auto const run = [&](sg::command_list& cmd)
    {
        cmd.upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(color_pixels).as_bytes());
        cmd.upload.bytes_to_texture(albedo.raw(), cc::span<tg::vec4f const>(albedo_pixels).as_bytes());
        cmd.upload.bytes_to_texture(normal.raw(), cc::span<tg::vec4f const>(normal_pixels).as_bytes());

        auto const in = sr::denoise_inputs{
            .color = color,
            .guides = {.albedo = albedo, .normal = normal},
            .output = output,
        };
        return sr::denoise_routine::execute(cmd, in, history, settings, false);
    };

    auto outcome = sr::denoise_outcome{};
    auto first_restarted = false;
    auto attempts = 0;
    for (auto attempt = 0; attempt < 8; ++attempt)
    {
        attempts = attempt + 1;
        auto cmd = ctx.create_command_list();
        outcome = run(*cmd);
        if (attempt == 0)
            first_restarted = outcome.restarted;
        REQUIRE(outcome.status != sr::denoise_status::unsupported);
        REQUIRE(outcome.status != sr::denoise_status::failed);
        ctx.submit_command_list(cc::move(cmd));

        if (outcome.is_denoised())
        {
            ctx.advance_epoch();
            break;
        }

        // The member's `init` built the network's pipelines, but against whichever shader library was live when this
        // context first prewarmed it — and every test in this binary brings its own — so on the first call here they
        // may still be compiling, on slib's queue rather than on anything an epoch advance drains.
        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
        ctx.advance_epoch();
    }

    REQUIRE(outcome.is_denoised())
        .context(cc::format("the member never produced a denoised frame; last status {}, first_restarted {}, attempts "
                            "{}",
                            int(outcome.status), first_restarted, attempts));
    CHECK(outcome.method == sr::denoise_method::oidn);
    CHECK(first_restarted).context("the first call on a fresh history starts from nothing");
    CHECK(history.method() == sr::denoise_method::oidn);
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

// What the network costs at a real resolution, which is the reason it cannot yet be pointed at one.
//
// Asked of a small network rather than a large one: the widths come from the weights and the extents are arithmetic,
// so the figure for 1080p is computable without allocating a byte of it.
ASYNC_INVOCABLE_TEST("sr - the OIDN network's memory is what stands between it and a real image",
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

    // `create` starts the pipelines compiling, and this test asks a question that never runs them.
    // Drained rather than abandoned, because work still carrying a finished test's context is what nexus reports.
    (void)co_await sr::impl::oidn_prewarm_pipelines(ctx);

    auto network = sr::impl::oidn_network();
    if (!network.create(ctx, tg::vec2i(64, 64)))
        SKIP("the network could not be created; sr's log says why");

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

    // The number this test exists to pin: one 1080p frame wants 2.7 GiB of feature maps, and 4K wants 10.7 GiB.
    // That is what makes tiling a prerequisite for pointing the member at a real image rather than an optimisation.
    //
    // This measures the UNTILED cost and will keep measuring it once tiles exist — a tile is what the network is
    // created at, so the figure below is exactly the reason a whole frame is not.
    CHECK(at_1080p > i64(2) * 1024 * 1024 * 1024).context(cc::format("1080p needs {} MiB of feature maps", mib(at_1080p)));

    co_return;
}

// Tiles against the whole image, which is the only thing that can price the overlap.
//
// The network is the same either way; what tiling changes is what each pixel could see while it was computed.
// So the question is entirely "is the overlap wide enough", and the answer is a comparison rather than an argument.
ASYNC_INVOCABLE_TEST("sr - the OIDN network in tiles agrees with the same image run whole",
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

    if (!sr::impl::oidn_weights_present())
        SKIP("the OIDN weights were not fetched (extern/oidn-weights/fetch-oidn-weights.py)");

    REQUIRE(co_await sr::impl::oidn_prewarm_pipelines(ctx)).context("the network's pipelines did not build");

    constexpr auto k_extent = 384;

    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = k_extent,
                                                 .height = k_extent,
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
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
    for (auto y = 0; y < k_extent; ++y)
        for (auto x = 0; x < k_extent; ++x)
        {
            auto const coarse = ((x / 24 + y / 24) % 2) == 0;
            auto const fine = ((x / 3 + y / 5) % 2) == 0;
            auto const a = coarse ? tg::vec4f(0.8f, 0.6f, 0.3f, 0) : tg::vec4f(0.1f, 0.2f, 0.5f, 0);
            auto const gradient = f32(x + y) / f32(2 * k_extent);
            auto const speckle = 0.3f + f32((x * 7 + y * 13) % 11) / 11.0f + (fine ? 0.2f : 0.0f);

            albedo_pixels.push_back(a);
            color_pixels.push_back(
                tg::vec4f(a[0] * speckle * (0.5f + gradient), a[1] * speckle, a[2] * speckle * (1.5f - gradient), 0));
            normal_pixels.push_back(tg::vec4f(0, 0, 1, 0));
        }

    auto whole = sr::impl::oidn_network();
    REQUIRE(whole.create(ctx, tg::vec2i(k_extent, k_extent), 512));
    CHECK(whole.tile_counts() == tg::vec2i(1, 1)).context("384 fits one 512 tile, so it should not be tiled at all");
    CHECK(whole.tile_overlap() == 0);

    // A 288 tensor over a 384 image, at the overlap the member actually uses, which is three tiles per axis.
    auto tiled = sr::impl::oidn_network();
    REQUIRE(tiled.create(ctx, tg::vec2i(k_extent, k_extent), 288));
    CHECK(tiled.padded_extent() == tg::vec2i(288, 288));
    CHECK(tiled.tile_counts()[0] > 1).context("a 384 image should not fit one 288 tile");
    CHECK(tiled.tile_overlap() == sr::impl::oidn_network::k_tile_overlap);

    // The whole point of tiling, restated as a number: the tensors are sized by the tile, not by the image.
    CHECK(tiled.feature_bytes() < whole.feature_bytes())
        .context(cc::format("tiled {} bytes vs whole {} bytes", tiled.feature_bytes(), whole.feature_bytes()));

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
    REQUIRE(a.size() == k_extent * k_extent);
    REQUIRE(b.size() == k_extent * k_extent);

    // Every pixel is written exactly once, so a gap between tiles shows up as an untouched texel rather than a seam.
    auto unwritten = 0;
    for (auto i = 0; i < b.size(); ++i)
        if (b[i][3] != 1.0f)
            ++unwritten;
    CHECK(unwritten == 0).context(cc::format("{} texels no tile wrote", unwritten));

    auto worst = 0.0;
    auto sum = 0.0;
    auto worst_at = tg::vec2i(0, 0);
    for (auto y = 0; y < k_extent; ++y)
        for (auto x = 0; x < k_extent; ++x)
            for (auto c = 0; c < 3; ++c)
            {
                auto const d = f64(tg::abs(a[y * k_extent + x][c] - b[y * k_extent + x][c]));
                sum += d;
                if (d > worst)
                {
                    worst = d;
                    worst_at = tg::vec2i(x, y);
                }
            }

    auto const mean = sum / f64(k_extent * k_extent * 3);

    // The bound the overlap is chosen against.
    // An overlap too narrow shows up here and nowhere else, and it shows up at a tile boundary rather than spread out.
    // Bounds set where the measurement put them, not where they felt safe.
    //
    // At this overlap the two runs agree bit for bit, so anything above noise here means the geometry moved.
    // For scale: the same comparison is 2.8e-01 out with no overlap, and 1.4e-03 out at an overlap of 64.
    CHECK(mean < 1.0e-6).context(cc::format("mean difference {}", mean));
    CHECK(worst < 1.0e-4)
        .context(cc::format("worst difference {} at {},{} (mean {})", worst, worst_at[0], worst_at[1], mean));

    co_return;
}
