#include "shader_fixtures.hh"
#include "texel_formats.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/bloom_routine.hh>
#include <typed-geometry/scalar/scalar.hh>

#ifdef SR_TEST_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#endif

using namespace cc::primitive_defines;

// sr::bloom_routine against its definition, `out = L - s S(L) + s (k * S(L))`.
//
// The exact form is computed here on the CPU, so the routine's mip chain answers to the definition and not to itself.

namespace
{
struct picture
{
    int width = 0;
    int height = 0;
    cc::vector<tg::vec4f> texels;

    [[nodiscard]] static picture filled(int width, int height, tg::vec4f value)
    {
        auto p = picture{.width = width, .height = height};
        for (auto i = 0; i < width * height; ++i)
            p.texels.push_back(value);
        return p;
    }

    void fill_box(int x0, int y0, int x1, int y1, tg::vec4f value)
    {
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x)
                texels[y * width + x] = value;
    }
};

[[nodiscard]] f32 luminance(tg::vec4f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

[[nodiscard]] f64 total_light(picture const& p)
{
    auto sum = 0.0;
    for (auto const& t : p.texels)
        sum += luminance(t);
    return sum;
}

/// The light of `c` that scatters: as the shader takes it.
[[nodiscard]] tg::vec4f scattered(tg::vec4f c, f32 knee)
{
    if (knee <= 0.0f)
        return c;
    auto const l = luminance(c);
    auto const width = 0.5f * knee;
    if (l <= knee - width)
        return tg::vec4f(0, 0, 0, 0);
    auto const excess = l >= knee + width ? l - knee : (l - knee + width) * (l - knee + width) / (4.0f * width);
    return c * (excess / l);
}

/// `p` blurred by a Gaussian of `sigma` pixels, mirrored at the edges.
[[nodiscard]] picture gaussian_blur(picture const& p, f64 sigma)
{
    auto const reach = int(tg::ceil(4.0 * sigma));
    auto weights = cc::vector<f32>();
    auto total = 0.0;
    for (auto i = -reach; i <= reach; ++i)
    {
        auto const w = tg::exp(-0.5 * f64(i) * f64(i) / (sigma * sigma));
        weights.push_back(f32(w));
        total += w;
    }
    for (auto& w : weights)
        w = f32(w / total);

    auto const mirrored = [](int i, int n)
    {
        auto const period = 2 * n;
        auto const m = ((i % period) + period) % period;
        return m < n ? m : period - 1 - m;
    };

    auto across = p;
    for (auto y = 0; y < p.height; ++y)
        for (auto x = 0; x < p.width; ++x)
        {
            auto sum = tg::vec4f(0, 0, 0, 0);
            for (auto i = -reach; i <= reach; ++i)
                sum += p.texels[y * p.width + mirrored(x + i, p.width)] * weights[i + reach];
            across.texels[y * p.width + x] = sum;
        }
    auto out = across;
    for (auto y = 0; y < p.height; ++y)
        for (auto x = 0; x < p.width; ++x)
        {
            auto sum = tg::vec4f(0, 0, 0, 0);
            for (auto i = -reach; i <= reach; ++i)
                sum += across.texels[mirrored(y + i, p.height) * p.width + x] * weights[i + reach];
            out.texels[y * p.width + x] = sum;
        }
    return out;
}

/// The definition, exactly; the radii and weights are the routine's kernel.
[[nodiscard]] picture exact_bloom(picture const& p, f32 strength, f32 knee)
{
    auto source = p;
    for (auto& t : source.texels)
        t = scattered(t, knee);

    auto spread = picture::filled(p.width, p.height, tg::vec4f(0, 0, 0, 0));
    constexpr f64 radii[] = {0.01, 0.03, 0.08, 0.2};
    constexpr f64 gains[] = {0.4, 0.3, 0.2, 0.1};
    for (auto g = 0; g < 4; ++g)
    {
        auto const blurred = gaussian_blur(source, radii[g] * p.height);
        for (auto i = 0; i < i32(spread.texels.size()); ++i)
            spread.texels[i] += blurred.texels[i] * f32(gains[g]);
    }

    auto out = p;
    for (auto i = 0; i < i32(out.texels.size()); ++i)
        out.texels[i] = p.texels[i] + (spread.texels[i] - source.texels[i]) * strength;
    return out;
}

cc::shared_async<picture> bloomed(sg::context& ctx, picture const& in, sr::bloom_settings settings, f32 exposure_scale = 1.0f)
{
    sr::bloom_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const format = sg::pixel_format::rgba32_float;
    auto const image
        = ctx.persistent.create_texture_2d({.format = format,
                                            .width = in.width,
                                            .height = in.height,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(image.raw(), sr_test::packed(format, in.texels));
    auto const out = sr::bloom_routine::execute(*cmd, image, settings, exposure_scale);
    REQUIRE(out.has_value()); // prewarmed above, so none here is a compile failure rather than a race
    auto const future = cmd->download.bytes_from_texture(out.value().raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const reading = future.bytes();
    auto const& bytes = co_await reading;
    co_return picture{.width = in.width,
                      .height = in.height,
                      .texels = sr_test::unpacked(sg::pixel_format::rgba16_float, bytes, isize(in.width) * in.height)};
}

/// The fraction of pixels' relative errors at or under which `share` of them lie.
[[nodiscard]] f64 relative_error_at(picture const& got, picture const& want, f64 share)
{
    auto errors = cc::vector<f64>();
    for (auto i = 0; i < i32(got.texels.size()); ++i)
        errors.push_back(tg::abs(f64(luminance(got.texels[i])) - luminance(want.texels[i]))
                         / cc::max(f64(luminance(want.texels[i])), 1e-4));
    cc::sort(errors);
    return errors[isize(f64(errors.size() - 1) * share)];
}
} // namespace

TEST("sr - bloom's level weights sum to one, and a taller image takes more levels")
{
    for (auto const height : {64, 720, 1080, 2160})
    {
        auto sum = 0.0;
        for (auto const w : sr::bloom_routine::level_weights(height, 1.0f))
            sum += w;
        CHECK(tg::abs(sum - 1.0) < 1e-5).dump("height", height);
    }
    CHECK(sr::bloom_routine::level_weights(2160, 1.0f).size() > sr::bloom_routine::level_weights(1080, 1.0f).size());

    // the widest Gaussian must lie inside the chain, or its share piles up on the last level
    auto const levels = i32(sr::bloom_routine::level_weights(2160, 1.0f).size());
    CHECK(sr::bloom_routine::level_sigma(levels - 1) >= 0.2 * 2160);
}

ASYNC_INVOCABLE_TEST("sr - bloom matches its definition, keeps total light, and leaves what is under the knee",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // A bright window in a dim room, off centre so the edges matter.
    // Tall enough that the kernel's narrowest Gaussian is wider than the chain's finest level, which a smaller image cannot represent.
    auto room = picture::filled(640, 360, tg::vec4f(0.05f, 0.05f, 0.05f, 1.0f));
    room.fill_box(120, 80, 220, 220, tg::vec4f(4.0f, 4.5f, 5.0f, 1.0f));

    auto const settings = sr::bloom_settings{.strength = 0.2f, .radius_scale = 1.0f, .knee = 1.0f};
    auto const out = co_await bloomed(ctx, room, settings);
    auto const exact = exact_bloom(room, settings.strength, settings.knee);

    // The chain is an approximation, so it answers to the final image, in most pixels, as libs/graphics/shaped-rendering/docs/bloom.md states.
    CHECK(relative_error_at(out, exact, 0.5) < 0.01);
    CHECK(relative_error_at(out, exact, 0.99) < 0.05);

    CHECK(tg::abs(total_light(out) / total_light(room) - 1.0) < 0.005);

    // With nothing above the knee nothing scatters, so an image the display can show comes back as it went in.
    auto const dim = picture::filled(64, 64, tg::vec4f(0.2f, 0.3f, 0.4f, 1.0f));
    auto const kept = co_await bloomed(ctx, dim, settings);
    for (auto i = 0; i < i32(dim.texels.size()); ++i)
        CHECK(tg::abs(kept.texels[i][1] - 0.3f) < 0.001f);

    // A knee of 0 scatters everything, and an even image stays even.
    auto const even = co_await bloomed(ctx, dim, {.strength = 0.5f, .knee = 0.0f});
    for (auto i = 0; i < i32(dim.texels.size()); ++i)
        CHECK(tg::abs(even.texels[i][1] - 0.3f) < 0.002f);

    // The knee is on light as shown, so showing the image four times brighter moves it down by that.
    auto const shown = co_await bloomed(ctx, room, {.strength = 0.2f, .knee = 4.0f}, 4.0f);
    CHECK(relative_error_at(shown, out, 0.99) < 0.002);
}

#ifdef SR_TEST_HAS_DX12
// What one image costs, in GPU time, at the two sizes a viewer lives at; a benchmark, which logs and asserts nothing.
ASYNC_BENCHMARK("sr - bloom, GPU time per image, on dx12")
{
    auto created = sg::create_dx12_context({});
    if (created.has_error())
        SKIP("no dx12 device");
    auto& ctx = *created.value();
    (void)sr_test::shader_fixtures();
    nx::allow_warnings("did not fit", "sg.dx12"); // the source images, once, before the clock starts
    sr::bloom_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    for (auto const size : {tg::vec2i(1920, 1080), tg::vec2i(3840, 2160)})
    {
        auto const format = sg::pixel_format::rgba32_float;
        auto const image
            = ctx.persistent.create_texture_2d({.format = format,
                                                .width = size[0],
                                                .height = size[1],
                                                .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
        auto bright = picture::filled(size[0], size[1], tg::vec4f(0.1f, 0.1f, 0.1f, 1.0f));
        bright.fill_box(size[0] / 4, size[1] / 4, size[0] / 2, size[1] / 2, tg::vec4f(4.0f, 4.0f, 4.0f, 1.0f));
        {
            auto cmd = ctx.create_command_list();
            cmd->upload.bytes_to_texture(image.raw(), sr_test::packed(format, bright.texels));
            ctx.submit_command_list(cc::move(cmd));
            co_await ctx.epochs_in_flight_completion(2);
            ctx.advance_epoch();
            co_await ctx.idle_completion();
        }

        auto spans = cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>();
        for (auto r = 0; r < 64; ++r)
        {
            auto cmd = ctx.create_command_list();
            auto const from = cmd->query.record_gpu_timestamp();
            auto const out = sr::bloom_routine::execute(*cmd, image, {.strength = 0.2f});
            REQUIRE(out.has_value());
            auto const to = cmd->query.record_gpu_timestamp();
            ctx.submit_command_list(cc::move(cmd));
            co_await ctx.epochs_in_flight_completion(2);
            ctx.advance_epoch();
            spans.push_back({from, to});
        }
        co_await ctx.idle_completion();

        auto ms = cc::vector<f64>();
        for (auto const& [from, to] : spans)
            ms.push_back(1e3 * (to.try_get_seconds().value() - from.try_get_seconds().value()));
        cc::sort(ms);
        CC_LOG_INFO("bloom at {}x{}: median {} ms, min {} ms", size[0], size[1], ms[ms.size() / 2], ms[0]);
    }
}
#endif
