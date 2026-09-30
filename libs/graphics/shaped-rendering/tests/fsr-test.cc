#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/fsr_upscale_routine.hh>
#include <shaped-rendering/reconstruct.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// sr's FSR 3.1 upscaler, run through sg, and the front composing it behind a denoiser.
//
// The image tests trace a known pattern at a low resolution with FSR's own jitter sequence, upscale it, and measure the
// result against the pattern evaluated at the output's pixel centres.
// Each one is a comparison between two runs rather than a threshold on one, so it pins a CONVENTION: flip the jitter's
// sign, or report no motion for a moving image, and the result is measurably worse.
// A convention is exactly what a run that merely completes cannot catch, since FSR renders something plausible either way.

static_assert(!std::is_copy_constructible_v<sr::upscale_history>, "copying a history would fork it");
static_assert(std::is_nothrow_move_constructible_v<sr::upscale_history>, "a history lives in per-view records that move");

namespace
{
constexpr int k_in = 32;
constexpr int k_out = 64;

constexpr auto image_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// The scene: stripes near the input's resolution limit, crossed by one hard diagonal edge, in [0, 1] on both axes.
/// Detail an upscaler can only recover from jittered frames.
[[nodiscard]] f32 scene(f32 u, f32 v)
{
    auto const stripes = 0.5f + 0.4f * tg::sin(tg::angle_f::make_from_degree(u * 360.0f * 11.0f));
    return u + v < 1.0f ? stripes : 0.15f + 0.6f * stripes;
}

/// One traced frame: every pixel sampled once at its centre plus `jitter`, the scene shifted right by `shift` input pixels.
[[nodiscard]] cc::vector<tg::vec4f> trace(tg::vec2f jitter, f32 shift)
{
    auto pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_in; ++y)
        for (auto x = 0; x < k_in; ++x)
        {
            auto const u = (f32(x) + 0.5f + jitter[0] - shift) / f32(k_in);
            auto const v = (f32(y) + 0.5f + jitter[1]) / f32(k_in);
            auto const s = scene(u, v);
            pixels.push_back(tg::vec4f(s, s, s, 1));
        }
    return pixels;
}

/// The scene at the output's pixel centres, shifted as the last traced frame was.
[[nodiscard]] f32 rmse_against_scene(cc::span<tg::vec4f const> pixels, f32 shift)
{
    // A border of four output pixels is left out: an upscaler has nothing to reconstruct an edge pixel from but clamps.
    auto sum = 0.0f;
    auto count = 0;
    for (auto y = 4; y < k_out - 4; ++y)
        for (auto x = 4; x < k_out - 4; ++x)
        {
            auto const u = (f32(x) + 0.5f) / f32(k_out) - shift / f32(k_in);
            auto const v = (f32(y) + 0.5f) / f32(k_out);
            auto const d = pixels[y * k_out + x][0] - scene(u, v);
            sum += d * d;
            ++count;
        }
    return tg::sqrt(sum / f32(count));
}

void upload(sg::command_list& cmd, sg::texture_2d const& tex, cc::span<tg::vec4f const> pixels)
{
    cmd.upload.bytes_to_texture(tex.raw(), pixels.as_bytes());
}

[[nodiscard]] cc::vector<tg::vec4f> filled(int size, tg::vec4f value)
{
    return cc::vector<tg::vec4f>::create_filled(size * size, value);
}

[[nodiscard]] sg::texture_2d make_image(sg::context& ctx, int size, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d({.format = format, .width = size, .height = size, .usage = image_usage});
}

cc::shared_async<cc::unit> prewarm(sg::context& ctx)
{
    sr::fsr_upscale_routine::prewarm(ctx);
    sr::atrous_denoise_routine::prewarm(ctx);
    sr::reconstruct_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();
}

/// How a run reports what it traced.
struct stream_options
{
    int frames = 24;
    f32 jitter_sign = 1.0f;     ///< -1 hands FSR the jitter the wrong way round
    f32 shift_per_frame = 0.0f; ///< input pixels the scene moves right each frame
    bool honest_motion = true;  ///< false reports a still image whatever moved
};

struct stream_result
{
    sr::upscale_outcome last;
    cc::vector<tg::vec4f> output;
    f32 last_shift = 0.0f;
};

/// Traces and upscales `options.frames` frames directly through the routine, and reads the last output back.
cc::shared_async<stream_result> run_stream(sg::context& ctx, stream_options options)
{
    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const depth = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const motion = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const output = make_image(ctx, k_out, sg::pixel_format::rgba32_float);

    auto history = sr::upscale_history();
    auto result = stream_result{};
    auto readback = sg::data_future<tg::vec4f>();
    for (auto frame = 0; frame < options.frames; ++frame)
    {
        auto const jitter = sr::fsr_upscale_routine::jitter(u32(frame), tg::vec2i(k_in, k_in), tg::vec2i(k_out, k_out));
        auto const shift = options.shift_per_frame * f32(frame);

        // This frame's pixel minus last frame's: a scene moving right moves every pixel right.
        auto const moved = frame > 0 && options.honest_motion ? options.shift_per_frame : 0.0f;

        auto cmd = ctx.create_command_list();
        upload(*cmd, color, trace(jitter, shift));
        upload(*cmd, depth, filled(k_in, tg::vec4f(1, 0, 0, 0)));
        upload(*cmd, motion, filled(k_in, tg::vec4f(moved, 0, 0, 0)));

        result.last = sr::fsr_upscale_routine::execute(
            *cmd,
            {.color = color, .depth = depth, .motion = motion, .jitter = jitter * options.jitter_sign, .output = output},
            history);
        if (frame + 1 == options.frames)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        result.last_shift = shift;
    }

    auto const pixels = co_await readback.data();
    for (auto i = isize(0); i < pixels.size(); ++i)
        result.output.push_back(pixels[i]);
    co_return result;
}

/// Whether the FSR image tests can run on `ctx`, and why not otherwise.
[[nodiscard]] cc::optional<cc::string_view> why_fsr_cannot_run(sg::context const& ctx)
{
    if (!sr::query_reconstruct_support(ctx).fsr)
        return cc::string_view("FSR does not run on this build or context");
    return {};
}
} // namespace

TEST("sr - fsr's ratios are its own quality modes")
{
    CHECK(sr::fsr_upscale_routine::ratio_of(sr::render_scale_preset::native) == 1.0f);
    CHECK(sr::fsr_upscale_routine::ratio_of(sr::render_scale_preset::quality) == 1.5f);
    CHECK(sr::fsr_upscale_routine::ratio_of(sr::render_scale_preset::balanced) == 1.7f);
    CHECK(sr::fsr_upscale_routine::ratio_of(sr::render_scale_preset::performance) == 2.0f);
}

TEST("sr - fsr's jitter covers the pixel and repeats with its phase count")
{
    if (!SR_HAS_FSR)
        SKIP("FSR's sources were not fetched into this build");

    // A 2x ratio cycles through 8 * 2^2 = 32 phases of Halton (2, 3).
    auto const in = tg::vec2i(k_in, k_in);
    auto const out = tg::vec2i(k_out, k_out);
    auto seen_left = false;
    auto seen_right = false;
    for (auto i = u32(0); i < 32; ++i)
    {
        auto const j = sr::fsr_upscale_routine::jitter(i, in, out);
        CHECK(tg::abs(j[0]) <= 0.5f);
        CHECK(tg::abs(j[1]) <= 0.5f);
        seen_left = seen_left || j[0] < -0.25f;
        seen_right = seen_right || j[0] > 0.25f;
    }
    CHECK(seen_left);
    CHECK(seen_right);
    CHECK(sr::fsr_upscale_routine::jitter(3, in, out) == sr::fsr_upscale_routine::jitter(35, in, out));
}

ASYNC_INVOCABLE_TEST("sr - an upscaler resolves behind a denoiser, and traces smaller", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    sg::context const& ctx = *ctx_h;
    if (!sr::query_reconstruct_support(ctx).fsr)
        SKIP("FSR does not run on this build or context");

    auto const out = tg::vec2i(640, 480);
    auto const performance = sr::reconstruct_settings{.denoiser = sr::denoise_method::atrous,
                                                      .scale = sr::render_scale_preset::performance};
    CHECK(sr::resolve_upscale_method(ctx, performance) == sr::upscale_method::fsr);
    CHECK(sr::reconstruct_input_extent(ctx, performance, out) == tg::vec2i(320, 240));
    CHECK(sr::reconstruct_jitter(ctx, performance, out, 5) != tg::vec2f(0, 0));

    // Automatic at a native scale has nothing to upscale, so a caller traces the output's size and does not jitter.
    auto const native = sr::reconstruct_settings{.denoiser = sr::denoise_method::atrous};
    CHECK(sr::resolve_upscale_method(ctx, native) == sr::upscale_method::none);
    CHECK(sr::reconstruct_input_extent(ctx, native, out) == out);
    CHECK(sr::reconstruct_jitter(ctx, native, out, 5) == tg::vec2f(0, 0));

    // Named explicitly, FSR runs at a native scale too, as anti-aliasing.
    auto const named
        = sr::reconstruct_settings{.denoiser = sr::denoise_method::atrous, .upscaler = sr::upscale_method::fsr};
    CHECK(sr::resolve_upscale_method(ctx, named) == sr::upscale_method::fsr);
    CHECK(sr::reconstruct_input_extent(ctx, named, out) == out);

    // A denoiser that upscales by itself has no upscaler behind it.
    auto const vendor = sr::reconstruct_settings{.denoiser = sr::denoise_method::dlss_rr,
                                                 .scale = sr::render_scale_preset::performance};
    CHECK(sr::resolve_upscale_method(ctx, vendor) == sr::upscale_method::none);

    // A denoiser this context cannot run gets the whole call refused, so a caller must not trace smaller or jitter for
    // the upscaler behind it.
    if (!sr::query_reconstruct_support(ctx).nrd)
    {
        auto const refused = sr::reconstruct_settings{.denoiser = sr::denoise_method::nrd,
                                                      .scale = sr::render_scale_preset::performance};
        CHECK(sr::reconstruct_input_extent(ctx, refused, out) == out);
        CHECK(sr::reconstruct_jitter(ctx, refused, out, 5) == tg::vec2f(0, 0));
    }
    co_return;
}

ASYNC_INVOCABLE_TEST("sr - fsr reconstructs from the jitter the right way round", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_fsr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    auto const honest = co_await run_stream(ctx, {});
    REQUIRE(honest.last.status == sr::reconstruct_status::denoised);
    auto const flipped = co_await run_stream(ctx, {.jitter_sign = -1.0f});
    REQUIRE(flipped.last.status == sr::reconstruct_status::denoised);

    // Measured on the development machine's GPU: 0.085 against 0.126 with the sign flipped.
    // A flipped sign places every sample on the wrong side of its pixel, which blurs the stripes and bends the edge.
    auto const fsr_error = rmse_against_scene(honest.output, 0.0f);
    auto const flipped_error = rmse_against_scene(flipped.output, 0.0f);
    CHECK(fsr_error < 0.8f * flipped_error).context(cc::format("fsr {}, flipped jitter {}", fsr_error, flipped_error));
}

ASYNC_INVOCABLE_TEST("sr - fsr follows a moving image through its motion vectors", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_fsr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    // The same drifting scene twice: told how it moved, and told nothing moved.
    // Only reprojection along the honest motion keeps the accumulated history on the stripes it came from.
    auto const honest = co_await run_stream(ctx, {.shift_per_frame = 0.75f});
    auto const still = co_await run_stream(ctx, {.shift_per_frame = 0.75f, .honest_motion = false});
    REQUIRE(honest.last.status == sr::reconstruct_status::denoised);

    // Measured on the development machine's GPU: 0.13 against 0.24.
    auto const honest_error = rmse_against_scene(honest.output, honest.last_shift);
    auto const still_error = rmse_against_scene(still.output, still.last_shift);
    CHECK(honest_error < 0.75f * still_error)
        .context(cc::format("honest motion {}, no motion {}", honest_error, still_error));
}

ASYNC_INVOCABLE_TEST("sr - the front upscales behind a denoiser, and restarts the upscaler when the denoiser changes",
                     (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_fsr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const depth = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const motion = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const output = make_image(ctx, k_out, sg::pixel_format::rgba32_float);

    auto history = sr::reconstruct_history();
    auto const run = [&](sr::denoise_method denoiser)
    {
        auto cmd = ctx.create_command_list();
        upload(*cmd, color, trace(tg::vec2f(0, 0), 0.0f));
        upload(*cmd, depth, filled(k_in, tg::vec4f(1, 0, 0, 0)));
        upload(*cmd, motion, filled(k_in, tg::vec4f(0, 0, 0, 0)));
        auto const outcome = sr::reconstruct_routine::execute(
            *cmd, {.color = color, .guides = {.depth = depth, .motion = motion}, .output = output}, history,
            {.denoiser = denoiser, .scale = sr::render_scale_preset::performance});
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        return outcome;
    };

    auto const first = run(sr::denoise_method::atrous);
    REQUIRE(first.status == sr::reconstruct_status::denoised);
    CHECK(first.denoiser == sr::denoise_method::atrous);
    CHECK(first.upscaler == sr::upscale_method::fsr);
    CHECK(first.restarted);

    CHECK(!run(sr::denoise_method::atrous).restarted);

    // Upscaling alone goes through the front too, and a new source image is a new history.
    auto const alone = run(sr::denoise_method::none);
    CHECK(alone.status == sr::reconstruct_status::denoised);
    CHECK(alone.denoiser == sr::denoise_method::none);
    CHECK(alone.restarted);
    co_return;
}

ASYNC_INVOCABLE_TEST("sr - an upscaler without depth and motion is refused and writes nothing",
                     (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_fsr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);
    nx::allow_warnings("upscaler 'fsr' did not run");

    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const output = make_image(ctx, k_out, sg::pixel_format::rgba32_float);
    auto const sentinel = filled(k_out, tg::vec4f(-7, -7, -7, -7));

    auto history = sr::reconstruct_history();
    auto cmd = ctx.create_command_list();
    upload(*cmd, color, trace(tg::vec2f(0, 0), 0.0f));
    upload(*cmd, output, sentinel);
    auto const outcome = sr::reconstruct_routine::execute(
        *cmd, {.color = color, .output = output}, history,
        {.denoiser = sr::denoise_method::atrous, .scale = sr::render_scale_preset::performance});
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    CHECK(outcome.status == sr::reconstruct_status::unsupported);
    CHECK(outcome.upscaler == sr::upscale_method::fsr);
    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_out * k_out);
    CHECK(pixels[0][0] == -7.0f);
}

// Needs no FSR, so it runs on every build: automatic at a native scale resolves to no upscaler anywhere.
ASYNC_INVOCABLE_TEST("sr - upscaling alone with nothing to upscale is refused and writes nothing",
                     (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    (void)sr_test::shader_fixtures();
    nx::allow_warnings("upscaler 'automatic' did not run");

    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const depth = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const motion = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const output = make_image(ctx, k_in, sg::pixel_format::rgba32_float);

    auto history = sr::reconstruct_history();
    auto cmd = ctx.create_command_list();
    upload(*cmd, output, filled(k_in, tg::vec4f(-7, -7, -7, -7)));
    auto const outcome = sr::reconstruct_routine::execute(
        *cmd, {.color = color, .guides = {.depth = depth, .motion = motion}, .output = output}, history,
        {.denoiser = sr::denoise_method::none, .upscaler = sr::upscale_method::automatic});
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    CHECK(outcome.status == sr::reconstruct_status::unsupported);
    CHECK(outcome.denoiser == sr::denoise_method::none);
    CHECK(outcome.upscaler == sr::upscale_method::none);
    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_in * k_in);
    CHECK(pixels[0][0] == -7.0f);
}

// WARP crashes executing FSR, so a software adapter must never be offered it; CI's WARP leg is what runs this.
ASYNC_INVOCABLE_TEST("sr - fsr is refused on a software adapter", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (!ctx.metrics.adapter().is_software)
        SKIP("the adapter is hardware");

    (void)sr_test::shader_fixtures();
    nx::allow_warnings("upscaler 'fsr' did not run");
    CHECK(!sr::query_reconstruct_support(ctx).fsr);

    auto const color = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const depth = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const motion = make_image(ctx, k_in, sg::pixel_format::rgba32_float);
    auto const output = make_image(ctx, k_out, sg::pixel_format::rgba32_float);

    auto history = sr::reconstruct_history();
    auto cmd = ctx.create_command_list();
    auto const outcome = sr::reconstruct_routine::execute(
        *cmd, {.color = color, .guides = {.depth = depth, .motion = motion}, .output = output}, history,
        {.denoiser = sr::denoise_method::none,
         .upscaler = sr::upscale_method::fsr,
         .scale = sr::render_scale_preset::performance});
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    CHECK(outcome.status == sr::reconstruct_status::unsupported);
    CHECK(outcome.upscaler == sr::upscale_method::fsr);
    co_return;
}
