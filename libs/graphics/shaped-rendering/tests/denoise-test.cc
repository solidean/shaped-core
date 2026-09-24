#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/svgf_denoise_routine.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// sr's denoise front and both of its native members.
//
// What these pin is the policy the front applies — which member `automatic` picks, and that a named member it cannot
// run is refused rather than replaced — plus each member's own behaviour.
// For à-trous, the three properties that make it safe to put on a converging mean: a flat image stays flat, an edge in
// the guides survives, and a deep mean is left almost alone.
// For SVGF, that a stream converges, that it follows a moving image through its motion vectors, and that a depth jump
// or an explicit reset drops the history rather than ghosting it.

static_assert(!std::is_copy_constructible_v<sr::denoise_history>, "copying a history would fork it");
static_assert(std::is_nothrow_move_constructible_v<sr::denoise_history>, "a history lives in per-view records that move");

namespace
{
constexpr int k_size = 32;

constexpr auto image_usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture
                           | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// A per-pixel pseudo-random value in [-1, 1], the same on every run.
[[nodiscard]] f32 noise_at(int x, int y)
{
    auto h = u32(x) * 73856093u ^ u32(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return f32(h & 0xffffu) / 32767.5f - 1.0f;
}

[[nodiscard]] sg::texture_2d make_image(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = k_size, .height = k_size, .usage = image_usage});
}

/// Two flat halves split at x == k_size / 2 — the image a denoiser should return.
[[nodiscard]] f32 clean_value(int x)
{
    return x < k_size / 2 ? 0.2f : 0.8f;
}

[[nodiscard]] cc::vector<tg::vec4f> clean_halves()
{
    auto pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const v = clean_value(x);
            pixels.push_back(tg::vec4f(v, v, v, 1));
        }
    return pixels;
}

/// The halves with noise of `amplitude` on top — a one-sample estimate of them.
[[nodiscard]] cc::vector<tg::vec4f> noisy_halves(f32 amplitude)
{
    auto pixels = clean_halves();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto& p = pixels[y * k_size + x];
            auto const v = p[0] + amplitude * noise_at(x, y);
            p = tg::vec4f(v, v, v, 1);
        }
    return pixels;
}

/// A normal guide with the same split as the halves, so the edge is in the geometry as well as the colour.
[[nodiscard]] cc::vector<tg::vec4f> split_normals()
{
    auto pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
            pixels.push_back(x < k_size / 2 ? tg::vec4f(1, 0, 0, 0) : tg::vec4f(0, 1, 0, 0));
    return pixels;
}

[[nodiscard]] f32 rmse_against_clean(cc::span<tg::vec4f const> pixels)
{
    auto sum = 0.0f;
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const d = pixels[y * k_size + x][0] - clean_value(x);
            sum += d * d;
        }
    return tg::sqrt(sum / f32(k_size * k_size));
}

void upload(sg::command_list& cmd, sg::texture_2d const& tex, cc::span<tg::vec4f const> pixels)
{
    cmd.upload.bytes_to_texture(tex.raw(), pixels.as_bytes());
}

/// Brings the à-trous member up before a list opens, so the first call does not decline.
cc::shared_async<cc::unit> prewarm(sg::context& ctx)
{
    sr::atrous_denoise_routine::prewarm(ctx);
    sr::svgf_denoise_routine::prewarm(ctx);
    sr::denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();
}

/// Uploads `color` (and `normals`, when given), runs one front call with `settings`, and reads the output back.
struct denoise_run
{
    sr::denoise_outcome outcome;
    cc::vector<tg::vec4f> output;
};

cc::shared_async<denoise_run> run_once(sg::context& ctx,
                                       cc::span<tg::vec4f const> color,
                                       cc::span<tg::vec4f const> normals,
                                       sr::denoise_settings settings,
                                       u32 sample_count,
                                       sr::denoise_history& history)
{
    auto const color_tex = make_image(ctx);
    auto const output_tex = make_image(ctx);
    auto normal_tex = sg::texture_2d();
    if (!normals.empty())
        normal_tex = make_image(ctx);

    // The output starts as a sentinel, so a call that should not have written anything is checkable.
    auto const sentinel = cc::vector<tg::vec4f>::create_filled(k_size * k_size, tg::vec4f(-7, -7, -7, -7));

    auto cmd = ctx.create_command_list();
    upload(*cmd, color_tex, color);
    upload(*cmd, output_tex, sentinel);
    if (!normals.empty())
        upload(*cmd, normal_tex, normals);

    auto const outcome = sr::denoise_routine::execute(*cmd,
                                                      {
                                                          .color = color_tex,
                                                          .guides = {.normal = normal_tex},
                                                          .output = output_tex,
                                                          .sample_count = sample_count,
                                                      },
                                                      history, settings);
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output_tex.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    auto result = denoise_run{.outcome = outcome};
    for (auto i = isize(0); i < pixels.size(); ++i)
        result.output.push_back(pixels[i]);
    co_return result;
}

constexpr auto atrous_settings = sr::denoise_settings{.method = sr::denoise_method::atrous};
} // namespace

ASYNC_INVOCABLE_TEST("sr - denoise automatic resolves to a supported member", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    sg::context const& ctx = *ctx_h;

    auto const support = sr::query_denoise_support(ctx);
    CHECK(support.atrous);

    // A caller feeding fresh frames gets the best temporal member.
    // One denoising a converging mean never does, since a temporal member's history would double-count what the mean
    // already averaged.
    auto const on_a_mean = sr::denoise_settings{.method = sr::denoise_method::automatic};
    auto const on_fresh_frames = sr::denoise_settings{.method = sr::denoise_method::automatic, .fresh_samples = true};
    CHECK(support.svgf);
    CHECK(sr::resolve_denoise_method(ctx, on_a_mean) == sr::denoise_method::atrous);
    CHECK(sr::resolve_denoise_method(ctx, on_fresh_frames) == sr::denoise_method::svgf);

    // A named member resolves to itself whether or not it is supported: refusing it is execute's job, and it must
    // not be quietly exchanged for another.
    auto const dlss = sr::denoise_settings{.method = sr::denoise_method::dlss_rr};
    CHECK(sr::resolve_denoise_method(ctx, dlss) == sr::denoise_method::dlss_rr);

    // Only the vendor members trace smaller than they output; every other member answers the output's own size.
    auto const scaled
        = sr::denoise_settings{.method = sr::denoise_method::atrous, .scale = sr::render_scale_preset::performance};
    CHECK(sr::denoise_input_extent(ctx, scaled, tg::vec2i(640, 480)) == tg::vec2i(640, 480));

    // ...and an upscaling member this device cannot run answers the output's own size too.
    // Otherwise a caller would trace at half resolution for a call that is about to be refused, and then composite
    // that half-resolution image into a full-resolution output.
    auto const dlss_scaled = sr::denoise_settings{.method = sr::denoise_method::dlss_rr,
                                                  .scale = sr::render_scale_preset::performance,
                                                  .fresh_samples = true};
    CHECK(!support.dlss_rr);
    CHECK(sr::denoise_input_extent(ctx, dlss_scaled, tg::vec2i(640, 480)) == tg::vec2i(640, 480));
    co_return;
}

ASYNC_INVOCABLE_TEST("sr - denoise refuses a named member it cannot run and writes nothing",
                     (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // Logged once per process, and it is the one line that tells a person why their image is still noisy.
    nx::allow_warnings("denoiser 'dlss_rr' did not run");

    auto history = sr::denoise_history();
    auto const run = co_await run_once(ctx, noisy_halves(0.1f), {}, {.method = sr::denoise_method::dlss_rr}, 1, history);
    CHECK(run.outcome.status == sr::denoise_status::unsupported);
    CHECK(run.outcome.method == sr::denoise_method::dlss_rr);
    REQUIRE(run.output.size() == k_size * k_size);
    CHECK(run.output[0][0] == -7.0f); // the sentinel survived: nothing else ran in its place
    CHECK(history.method() == sr::denoise_method::none);
}

ASYNC_INVOCABLE_TEST("sr - atrous keeps a flat image flat", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // Every weight is normalized, so averaging equal values must return that value exactly — up to float rounding.
    auto const flat = cc::vector<tg::vec4f>::create_filled(k_size * k_size, tg::vec4f(0.37f, 0.5f, 1.25f, 1));
    auto history = sr::denoise_history();
    auto const run = co_await run_once(ctx, flat, {}, atrous_settings, 1, history);
    REQUIRE(run.outcome.status == sr::denoise_status::denoised);
    CHECK(run.outcome.method == sr::denoise_method::atrous);

    auto worst = 0.0f;
    for (auto const& p : run.output)
        worst = cc::max(worst, cc::max(tg::abs(p[0] - 0.37f), cc::max(tg::abs(p[1] - 0.5f), tg::abs(p[2] - 1.25f))));
    CHECK(worst < 1e-5f);
}

ASYNC_INVOCABLE_TEST("sr - atrous removes noise without bleeding across a guide edge", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    auto const noisy = noisy_halves(0.1f);
    auto history = sr::denoise_history();
    auto const run = co_await run_once(ctx, noisy, split_normals(), atrous_settings, 1, history);
    REQUIRE(run.outcome.status == sr::denoise_status::denoised);

    // Well under half the input's error: a filter that did nothing, or blurred the edge away, fails this.
    auto const before = rmse_against_clean(noisy);
    auto const after = rmse_against_clean(run.output);
    CHECK(after < 0.5f * before);

    // The two columns either side of the edge are where bleeding would show: each must stay on its own side's value.
    // The normal guide is what stops the taps crossing, since the luminance difference alone is only a few noise widths.
    for (auto y = 0; y < k_size; ++y)
    {
        CHECK(tg::abs(run.output[y * k_size + k_size / 2 - 1][0] - 0.2f) < 0.1f);
        CHECK(tg::abs(run.output[y * k_size + k_size / 2][0] - 0.8f) < 0.1f);
    }
}

ASYNC_INVOCABLE_TEST("sr - atrous leaves a deep mean almost alone", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // The same pixels, claimed to average 4096 samples each: the noise a mean that deep carries is 1/64 of a single
    // sample's, so what differs between neighbours is taken for detail and kept.
    // This is what lets the denoiser sit on sv's accumulating mean without softening the converged image.
    auto const noisy = noisy_halves(0.1f);
    auto history = sr::denoise_history();
    auto const run = co_await run_once(ctx, noisy, {}, atrous_settings, 4096, history);
    REQUIRE(run.outcome.status == sr::denoise_status::denoised);

    // Measured against the noise it would otherwise have removed: an RMS movement of a fifth of the input's own
    // deviation means detail is kept, where the one-sample test above removes well over half of it.
    auto moved_sq = 0.0f;
    for (auto i = isize(0); i < run.output.size(); ++i)
    {
        auto const d = run.output[i][0] - noisy[i][0];
        moved_sq += d * d;
    }
    auto const moved = tg::sqrt(moved_sq / f32(run.output.size()));
    CHECK(moved < 0.2f * rmse_against_clean(noisy));
}

ASYNC_INVOCABLE_TEST("sr - denoise history restarts on first use and after a reset", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    auto const noisy = noisy_halves(0.1f);
    auto history = sr::denoise_history();

    auto const first = co_await run_once(ctx, noisy, {}, atrous_settings, 1, history);
    CHECK(first.outcome.restarted);
    CHECK(history.method() == sr::denoise_method::atrous);
    CHECK(history.extent() == tg::vec2i(k_size, k_size));

    auto const second = co_await run_once(ctx, noisy, {}, atrous_settings, 1, history);
    CHECK(!second.outcome.restarted);

    history.reset();
    auto const after_reset = co_await run_once(ctx, noisy, {}, atrous_settings, 1, history);
    CHECK(after_reset.outcome.restarted);

    auto const again = co_await run_once(ctx, noisy, {}, atrous_settings, 1, history);
    CHECK(!again.outcome.restarted);
}

namespace
{
/// The halves with fresh noise for `frame`, as one frame of a stream of independent one-sample estimates.
[[nodiscard]] cc::vector<tg::vec4f> noisy_frame(int frame, f32 amplitude)
{
    auto pixels = clean_halves();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto& p = pixels[y * k_size + x];
            auto const v = p[0] + amplitude * noise_at(x + frame * 101, y + frame * 57);
            p = tg::vec4f(v, v, v, 1);
        }
    return pixels;
}

/// A fixed per-column value in [0.15, 0.85], the same on every run and different for every column.
///
/// High spatial frequency on purpose: a moving-stream test needs an image where following the motion actually
/// changes which value a pixel finds, and two flat halves do not — a pixel deep inside one of them has the same
/// value before and after a shift, so a reprojection that went nowhere would look identical.
[[nodiscard]] f32 clean_column(int x)
{
    auto h = u32(x + 4096) * 2654435761u;
    h ^= h >> 15;
    h *= 0x2545f491u;
    h ^= h >> 13;
    return 0.15f + 0.7f * f32(h & 0xffffu) / 65535.0f;
}

/// The column pattern translated `shift` pixels right, with fresh noise for `frame`.
[[nodiscard]] cc::vector<tg::vec4f> shifted_columns(int frame, int shift, f32 amplitude)
{
    auto pixels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const v = clean_column(x - shift) + amplitude * noise_at(x + frame * 101, y + frame * 57);
            pixels.push_back(tg::vec4f(v, v, v, 1));
        }
    return pixels;
}

/// The error of a shifted stream's output against the pattern it should have converged to.
///
/// The columns a shift has pulled in from off-screen are skipped: they have no history behind them yet, however
/// correct the reprojection is.
[[nodiscard]] f32 rmse_against_shifted(cc::span<tg::vec4f const> pixels, int shift)
{
    auto sum = 0.0f;
    auto count = 0;
    for (auto y = 0; y < k_size; ++y)
        for (auto x = shift; x < k_size; ++x)
        {
            auto const d = pixels[y * k_size + x][0] - clean_column(x - shift);
            sum += d * d;
            ++count;
        }
    return tg::sqrt(sum / f32(count));
}

/// A texture's worth of one value everywhere.
[[nodiscard]] cc::vector<tg::vec4f> filled(tg::vec4f v)
{
    return cc::vector<tg::vec4f>::create_filled(k_size * k_size, v);
}

/// The images one temporal stream keeps across its frames: the caller's, as sv would hold them.
struct svgf_stream
{
    sg::texture_2d color;
    sg::texture_2d normal;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d output;
    sr::denoise_history history;
};

[[nodiscard]] svgf_stream make_stream(sg::context& ctx)
{
    return {.color = make_image(ctx),
            .normal = make_image(ctx),
            .depth = make_image(ctx),
            .motion = make_image(ctx),
            .output = make_image(ctx)};
}

/// One frame of `stream`: uploads `color` with the given normals, depth and motion, denoises through the front as a
/// caller feeding fresh samples, and reads the output back.
cc::shared_async<denoise_run> stream_frame(sg::context& ctx,
                                           svgf_stream& stream,
                                           cc::span<tg::vec4f const> color,
                                           f32 depth,
                                           tg::vec2f motion = tg::vec2f(0, 0),
                                           cc::span<tg::vec4f const> normals = {})
{
    auto const own_normals = normals.empty() ? split_normals() : cc::vector<tg::vec4f>();
    auto const guide_normals = normals.empty() ? cc::span<tg::vec4f const>(own_normals) : normals;

    auto cmd = ctx.create_command_list();
    upload(*cmd, stream.color, color);
    upload(*cmd, stream.normal, guide_normals);
    upload(*cmd, stream.depth, filled(tg::vec4f(depth, 0, 0, 0)));
    upload(*cmd, stream.motion, filled(tg::vec4f(motion[0], motion[1], 0, 0)));

    auto const outcome = sr::denoise_routine::execute(
        *cmd,
        {.color = stream.color,
         .guides = {.normal = stream.normal, .depth = stream.depth, .motion = stream.motion},
         .output = stream.output},
        stream.history, {.method = sr::denoise_method::svgf, .fresh_samples = true});
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(stream.output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    auto result = denoise_run{.outcome = outcome};
    for (auto i = isize(0); i < pixels.size(); ++i)
        result.output.push_back(pixels[i]);
    co_return result;
}

[[nodiscard]] f32 max_distance_from(cc::span<tg::vec4f const> pixels, f32 value)
{
    auto worst = 0.0f;
    for (auto const& p : pixels)
        worst = cc::max(worst, tg::abs(p[0] - value));
    return worst;
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - svgf converges a static noisy stream", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // Eight independent one-sample frames of a still image.
    // Each frame's noise is fresh, so the history averages it away: the error has to fall as the stream goes on, well
    // below what one frame alone gets to.
    auto stream = make_stream(ctx);
    auto first_error = 0.0f;
    auto last_error = 0.0f;
    for (auto frame = 0; frame < 8; ++frame)
    {
        auto const run = co_await stream_frame(ctx, stream, noisy_frame(frame, 0.1f), 1.0f);
        REQUIRE(run.outcome.status == sr::denoise_status::denoised);
        CHECK(run.outcome.method == sr::denoise_method::svgf);
        CHECK(run.outcome.restarted == (frame == 0));
        if (frame == 0)
            first_error = rmse_against_clean(run.output);
        last_error = rmse_against_clean(run.output);
    }

    CHECK(last_error < 0.6f * first_error);
    CHECK(last_error < 0.25f * rmse_against_clean(noisy_frame(0, 0.1f)));
}

ASYNC_INVOCABLE_TEST("sr - svgf follows a moving image through its motion vectors", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // A high-frequency column pattern translated one pixel right per frame, with a motion vector saying exactly that.
    // A motion vector is this frame's pixel minus last frame's, so a surface that moved +1 in x carries (1, 0).
    //
    // The normals and the depth are flat everywhere, so the temporal pass's surface test accepts every reprojection
    // and the ONLY thing separating the two runs below is which texel each pixel reads its history from.
    //
    // This is what a stream of zero motion cannot pin: with every pixel reading from its own position, a flipped
    // sign, a missing half-pixel offset or a mis-wired motion binding all still converge.
    auto const flat_normals = filled(tg::vec4f(0, 0, 1, 0));

    auto with_motion = make_stream(ctx);
    auto with_motion_last = 0.0f;
    for (auto frame = 0; frame < 8; ++frame)
    {
        auto const run = co_await stream_frame(ctx, with_motion, shifted_columns(frame, frame, 0.1f), 1.0f,
                                               tg::vec2f(1, 0), flat_normals);
        REQUIRE(run.outcome.status == sr::denoise_status::denoised);
        CHECK(run.outcome.restarted == (frame == 0));
        with_motion_last = rmse_against_shifted(run.output, frame);
    }

    // The same stream told nothing moved: each pixel then blends the history of the column that used to be under it,
    // which is a different column's value, and the result is an average across the pattern rather than a mean of it.
    auto without_motion = make_stream(ctx);
    auto without_motion_last = 0.0f;
    for (auto frame = 0; frame < 8; ++frame)
    {
        auto const run = co_await stream_frame(ctx, without_motion, shifted_columns(frame, frame, 0.1f), 1.0f,
                                               tg::vec2f(0, 0), flat_normals);
        REQUIRE(run.outcome.status == sr::denoise_status::denoised);
        without_motion_last = rmse_against_shifted(run.output, frame);
    }

    // Following the motion is the whole mechanism, so it must converge substantially further.
    //
    // 0.6 rather than something tighter: the defects this exists to catch — a flipped sign, a missing half-pixel
    // offset, a motion texture bound to the wrong slot — do not shave the ratio, they remove reprojection, so the
    // result collapses toward the told-nothing-moved figure.
    // Anything between the two values catches all three, and the slack is what keeps a float-rounding difference
    // between two backends or two drivers from failing a test that is measuring a factor of two.
    CHECK(with_motion_last < 0.6f * without_motion_last);

    // And it must reach the same place a stream that never moved does: that is what reprojection buys.
    auto still = make_stream(ctx);
    auto still_last = 0.0f;
    for (auto frame = 0; frame < 8; ++frame)
    {
        auto const run
            = co_await stream_frame(ctx, still, shifted_columns(frame, 0, 0.1f), 1.0f, tg::vec2f(0, 0), flat_normals);
        REQUIRE(run.outcome.status == sr::denoise_status::denoised);
        still_last = rmse_against_shifted(run.output, 0);
    }
    CHECK(with_motion_last < 2.0f * still_last);
}

ASYNC_INVOCABLE_TEST("sr - svgf drops the history where the depth jumped, and after a reset",
                     (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    // A few frames of one flat value build a history of it.
    auto stream = make_stream(ctx);
    for (auto frame = 0; frame < 4; ++frame)
        (void)co_await stream_frame(ctx, stream, filled(tg::vec4f(0.2f, 0.2f, 0.2f, 1)), 1.0f);

    // The depth doubles everywhere with no motion: a different surface now sits behind every pixel, and blending the
    // old value in would ghost it.
    // Every pixel must show only the new value, rather than a blend weighted toward the history.
    auto const jumped = co_await stream_frame(ctx, stream, filled(tg::vec4f(0.8f, 0.8f, 0.8f, 1)), 2.0f);
    REQUIRE(jumped.outcome.status == sr::denoise_status::denoised);
    CHECK(max_distance_from(jumped.output, 0.8f) < 1e-3f);

    // A reset is the caller saying the same thing — a camera cut — where the geometry does not show it.
    for (auto frame = 0; frame < 4; ++frame)
        (void)co_await stream_frame(ctx, stream, filled(tg::vec4f(0.8f, 0.8f, 0.8f, 1)), 2.0f);
    stream.history.reset();
    auto const after_reset = co_await stream_frame(ctx, stream, filled(tg::vec4f(0.3f, 0.3f, 0.3f, 1)), 2.0f);
    CHECK(after_reset.outcome.restarted);
    CHECK(max_distance_from(after_reset.output, 0.3f) < 1e-3f);
}

ASYNC_INVOCABLE_TEST("sr - denoise refuses svgf without a motion guide", (sg::context_handle const& ctx_h), )
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    co_await prewarm(ctx);

    nx::allow_warnings("denoiser 'svgf' did not run");

    // A required guide that is missing is refused at the front, before the member would assert on it.
    auto history = sr::denoise_history();
    auto const run = co_await run_once(ctx, noisy_halves(0.1f), split_normals(),
                                       {.method = sr::denoise_method::svgf, .fresh_samples = true}, 1, history);
    CHECK(run.outcome.status == sr::denoise_status::unsupported);
    CHECK(run.outcome.method == sr::denoise_method::svgf);
    CHECK(run.output[0][0] == -7.0f);
}
