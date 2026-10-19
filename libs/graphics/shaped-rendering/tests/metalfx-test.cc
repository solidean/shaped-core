#include "shader_fixtures.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/metalfx_upscale_routine.hh>
#include <shaped-rendering/reconstruct.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// sr's MetalFX upscaler, encoded through sg's Metal native scope, and the front composing it behind a denoiser.
//
// The image tests are fsr-test's, run against MetalFX: a known pattern traced at a low resolution with the member's own
// jitter sequence, upscaled, and measured against the pattern at the output's pixel centres.
// Each is a comparison between two runs, so it pins a CONVENTION — the jitter's sign, the motion's direction — which a
// run that merely completes cannot catch, since MetalFX renders something plausible either way.
// The scene, tracer and metric are copies of fsr-test's, which builds only where DXC does.

namespace
{
constexpr int k_in = 32;
constexpr int k_out = 64;

constexpr auto image_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// Stripes near the input's resolution limit, crossed by one hard diagonal edge, in [0, 1] on both axes.
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

/// The error against the scene at the output's pixel centres, shifted as the last traced frame was.
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
    sr::metalfx_upscale_routine::prewarm(ctx);
    sr::atrous_denoise_routine::prewarm(ctx, sg::pixel_format::rgba32_float);
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba32_float);
    (void)co_await ctx.routines.idle_completion();
}

struct stream_options
{
    int frames = 24;
    f32 jitter_sign = 1.0f;         ///< -1 hands MetalFX the jitter the wrong way round
    f32 shift_per_frame = 0.0f;     ///< input pixels the scene moves right each frame
    bool honest_motion = true;      ///< false reports a still image whatever moved
    bool renderable_output = false; ///< an output MetalFX may write itself, rather than through the routine's copy
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
    auto const output = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = k_out,
         .height = k_out,
         .usage = options.renderable_output ? image_usage | sg::texture_usage::render_target : image_usage});

    auto history = sr::upscale_history();
    auto result = stream_result{};
    auto readback = sg::data_future<tg::vec4f>();
    for (auto frame = 0; frame < options.frames; ++frame)
    {
        auto const jitter
            = sr::metalfx_upscale_routine::jitter(u32(frame), tg::vec2i(k_in, k_in), tg::vec2i(k_out, k_out));
        auto const shift = options.shift_per_frame * f32(frame);

        // This frame's pixel minus last frame's: a scene moving right moves every pixel right.
        auto const moved = frame > 0 && options.honest_motion ? options.shift_per_frame : 0.0f;

        auto cmd = ctx.create_command_list();
        upload(*cmd, color, trace(jitter, shift));
        upload(*cmd, depth, filled(k_in, tg::vec4f(1, 0, 0, 0)));
        upload(*cmd, motion, filled(k_in, tg::vec4f(moved, 0, 0, 0)));

        result.last = sr::metalfx_upscale_routine::execute(
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

[[nodiscard]] cc::optional<cc::string_view> why_metalfx_cannot_run(sg::context const& ctx)
{
    if (!sr::query_reconstruct_support(ctx).metalfx)
        return cc::string_view("MetalFX does not run on this build or context");
    return {};
}
} // namespace

TEST("sr - metalfx's ratios are fsr's quality modes")
{
    CHECK(sr::metalfx_upscale_routine::ratio_of(sr::render_scale_preset::native) == 1.0f);
    CHECK(sr::metalfx_upscale_routine::ratio_of(sr::render_scale_preset::quality) == 1.5f);
    CHECK(sr::metalfx_upscale_routine::ratio_of(sr::render_scale_preset::balanced) == 1.7f);
    CHECK(sr::metalfx_upscale_routine::ratio_of(sr::render_scale_preset::performance) == 2.0f);
}

TEST("sr - metalfx's jitter covers the pixel and repeats with its phase count")
{
    // A 2x ratio cycles through 8 * 2^2 = 32 phases of Halton (2, 3).
    auto const in = tg::vec2i(k_in, k_in);
    auto const out = tg::vec2i(k_out, k_out);
    auto seen_left = false;
    auto seen_right = false;
    for (auto i = u32(0); i < 32; ++i)
    {
        auto const j = sr::metalfx_upscale_routine::jitter(i, in, out);
        CHECK(tg::abs(j[0]) <= 0.5f);
        CHECK(tg::abs(j[1]) <= 0.5f);
        seen_left = seen_left || j[0] < -0.25f;
        seen_right = seen_right || j[0] > 0.25f;
    }
    CHECK(seen_left);
    CHECK(seen_right);
    CHECK(sr::metalfx_upscale_routine::jitter(3, in, out) == sr::metalfx_upscale_routine::jitter(35, in, out));
}

ASYNC_INVOCABLE_TEST("sr - automatic picks metalfx on metal, and traces smaller", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    sg::context const& ctx = *ctx_h;
    if (auto const why = why_metalfx_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    auto const out = tg::vec2i(640, 480);
    auto const performance = sr::reconstruct_settings{.denoiser = sr::denoise_method::atrous,
                                                      .scale = sr::render_scale_preset::performance};
    CHECK(sr::resolve_upscale_method(ctx, performance, sr_test::every_guide()) == sr::upscale_method::metalfx);
    CHECK(sr::reconstruct_input_extent(ctx, performance, out, sr_test::every_guide()) == tg::vec2i(320, 240));
    CHECK(sr::reconstruct_jitter(ctx, performance, out, 5, sr_test::every_guide())
          == sr::metalfx_upscale_routine::jitter(5, tg::vec2i(320, 240), out));
    co_return;
}

ASYNC_INVOCABLE_TEST("sr - metalfx reconstructs from the jitter the right way round",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_metalfx_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    auto const honest = co_await run_stream(ctx, {});
    REQUIRE(honest.last.status == sr::reconstruct_status::denoised);
    auto const flipped = co_await run_stream(ctx, {.jitter_sign = -1.0f});
    REQUIRE(flipped.last.status == sr::reconstruct_status::denoised);

    // Measured on an M4: 0.044 against 0.181 with the sign flipped.
    auto const error = rmse_against_scene(honest.output, 0.0f);
    auto const flipped_error = rmse_against_scene(flipped.output, 0.0f);
    CHECK(error < 0.8f * flipped_error).context(cc::format("metalfx {}, flipped jitter {}", error, flipped_error));
}

ASYNC_INVOCABLE_TEST("sr - metalfx follows a moving image through its motion vectors",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_metalfx_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    // The same drifting scene twice: told how it moved, and told nothing moved.
    auto const honest = co_await run_stream(ctx, {.shift_per_frame = 0.75f});
    auto const still = co_await run_stream(ctx, {.shift_per_frame = 0.75f, .honest_motion = false});
    REQUIRE(honest.last.status == sr::reconstruct_status::denoised);

    auto const honest_error = rmse_against_scene(honest.output, honest.last_shift);
    auto const still_error = rmse_against_scene(still.output, still.last_shift);
    CHECK(honest_error < 0.75f * still_error)
        .context(cc::format("honest motion {}, no motion {}", honest_error, still_error));
}

ASYNC_INVOCABLE_TEST("sr - metalfx writes a renderable output itself, and the same image as through its copy",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_metalfx_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    co_await prewarm(ctx);

    auto const direct = co_await run_stream(ctx, {.renderable_output = true});
    auto const copied = co_await run_stream(ctx, {});
    REQUIRE(direct.last.status == sr::reconstruct_status::denoised);
    REQUIRE(copied.last.status == sr::reconstruct_status::denoised);

    auto largest = 0.0f;
    for (auto i = isize(0); i < direct.output.size(); ++i)
        largest = cc::max(largest, tg::abs(direct.output[i][0] - copied.output[i][0]));
    CHECK(largest < 1e-3f).context(cc::format("largest difference {}", largest));
}

ASYNC_INVOCABLE_TEST("sr - the front upscales through metalfx behind a denoiser",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_metalfx_cannot_run(ctx); why.has_value())
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
    CHECK(first.upscaler == sr::upscale_method::metalfx);
    CHECK(first.restarted);
    CHECK(!run(sr::denoise_method::atrous).restarted);
    co_await ctx.idle_completion();
}
