#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/nrd_instance.hh>
#include <shaped-rendering/nrd_denoise_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/scalar/scalar.hh>

#if SR_HAS_NRD
#include <shaped-rendering/impl/nrd_session.hh>
#endif

using namespace cc::primitive_defines;

// NRD's dispatch list, executed through sg.
//
// NRD renders nothing itself: it reports the pipelines it needs, the scratch it needs, and then a list of dispatches
// over those plus ours.
// So what there is to get wrong is entirely on our side — the register layout the pipelines are built against, the
// pool textures, and whether each dispatch's resources reach the bindings it declared.
//
// Every one of those failures is a debug-layer error rather than a wrong number, which is why this test asserts so
// little and still means something: the log rule fails it on any validation message, and reaching the end means the
// whole frame recorded and ran.
//
// It needs no particular adapter.
// NRD's dispatches are ordinary compute, so unlike DLSS this runs on WARP too — which is the whole reason this member
// is the one CI could test.
#if SR_HAS_NRD
ASYNC_INVOCABLE_TEST("sr - an NRD frame's dispatches build and run", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto const extent = tg::vec2i(64, 48);

    auto session = sr::impl::nrd_session();
    REQUIRE(session.create(ctx, sr::impl::nrd_denoiser::reblur_diffuse_specular, extent));
    CHECK(session.is_valid());
    CHECK(session.extent() == extent);

    // The pipelines build in the background, so a session is ready some time after it is created.
    // Waited on the context's own backlog, which is what a pipeline build is queued on — a condition rather than a
    // duration.
    cc::async_backlog const* const backlogs[] = {&ctx.backlog};
    co_await cc::async_settled(cc::async_backlog::settled(backlogs));
    REQUIRE(session.is_ready()).context("NRD's pipelines never finished building");

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format,
             .width = extent[0],
             .height = extent[1],
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst});
    };

    // Zeroed inputs in NRD's own formats.
    // What is under test is the plumbing rather than the picture: a denoiser handed an empty scene has nothing to say,
    // and asserting on what it said would be asserting on NRD.
    auto const resources = sr::impl::nrd_resources{
        .motion = make(sg::pixel_format::rgba16_float),
        .normal_roughness = make(sg::pixel_format::rgb10a2_unorm),
        .view_z = make(sg::pixel_format::r32_float),
        .diffuse_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .specular_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .out_diffuse_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
        .out_specular_radiance_hit_distance = make(sg::pixel_format::rgba16_float),
    };

    // Two frames: the first resets the history and the second continues it, which are different dispatch lists.
    // One frame alone would never exercise the reprojecting half, which is most of what REBLUR is.
    for (auto frame_index = u32(0); frame_index < 2; ++frame_index)
    {
        auto cmd = ctx.create_command_list();
        auto const frame = sr::impl::nrd_frame{.frame_index = frame_index, .reset = frame_index == 0};
        CHECK(session.execute(*cmd, frame, resources)).context(cc::format("frame {}", frame_index));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
    }

    (void)co_await ctx.idle_completion();
}

// The shared quality knob reaches REBLUR, rather than being accepted and dropped.
//
// `denoise_settings::quality` says every member reads it, and a member that quietly ignores it is a setting that
// looks live and does nothing — which is how `nrd_options::exposure` got written before NRD's own contract turned out
// to forbid it.
// Checked at the mapping rather than on an image: how long a history may grow is not visible in two frames of a
// constant field, and a test that ran the denoiser here would pass whatever the numbers were.
TEST("sr - NRD's quality preset picks how long its history may grow")
{
    auto const fast = sr::nrd_denoise_routine::options_for({.quality = sr::denoise_quality::fast});
    auto const balanced = sr::nrd_denoise_routine::options_for({.quality = sr::denoise_quality::balanced});
    auto const best = sr::nrd_denoise_routine::options_for({.quality = sr::denoise_quality::best});

    CHECK(fast.max_accumulated_frames < balanced.max_accumulated_frames);
    CHECK(balanced.max_accumulated_frames < best.max_accumulated_frames);
    CHECK(fast.max_fast_accumulated_frames < balanced.max_fast_accumulated_frames);
    CHECK(balanced.max_fast_accumulated_frames < best.max_fast_accumulated_frames);

    // REBLUR refuses a history longer than its own ceiling, which would fail the whole frame rather than clamp.
    CHECK(best.max_accumulated_frames <= 63);
}

// A session that was never created runs nothing, rather than dispatching against a null instance.
TEST("sr - an uncreated NRD session is inert")
{
    auto session = sr::impl::nrd_session();
    CHECK(!session.is_valid());
    CHECK(!session.is_ready());
}
// The member, end to end: guides in, one denoised image out.
//
// Both tests below light one flat surface uniformly and hand NRD what a tracer would have produced for it, so the
// answer is known without reimplementing REBLUR.
// Radiance picked free of the albedo it supposedly came off — a specular of 0.3 from an F0 of 0.04 — de-modulates to
// twenty times what any real frame carries, and REBLUR then reshapes a signal no tracer would have handed it.

namespace
{
/// Big enough that REBLUR's filters are local to it.
/// Its history-fix pass samples at a stride of 14 texels and its blur radius reaches 30, so on a 16-texel image every
/// pixel mixes the whole picture and nothing about the de-modulation can be told from its absence.
constexpr auto k_size = 64;

constexpr auto k_irradiance = 0.5f;
constexpr auto k_specular_radiance = 0.02f;
constexpr auto k_specular_albedo = 0.04f;

/// Denoises one flat, uniformly lit surface whose diffuse albedo is `albedo`, and reads the result back.
///
/// The radiance follows the albedo, which is what a uniformly lit surface produces and what makes the de-modulated
/// signal the thing REBLUR is actually meant to see.
[[nodiscard]] cc::shared_async<cc::vector<tg::vec4f>> denoise_lit_surface(sg::context& ctx, cc::vector<tg::vec4f> albedo)
{
    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const diffuse = make(sg::pixel_format::rgba32_float);
    auto const specular = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const roughness = make(sg::pixel_format::rgba32_float);
    auto const depth = make(sg::pixel_format::rgba32_float);
    auto const motion = make(sg::pixel_format::rgba32_float);
    auto const hit_distance = make(sg::pixel_format::rg32_float);
    auto const albedo_texture = make(sg::pixel_format::rgba32_float);
    auto const specular_albedo = make(sg::pixel_format::rgba32_float);
    auto const output = make(sg::pixel_format::rgba32_float);

    auto lit = cc::vector<tg::vec4f>();
    lit.reserve(albedo.size());
    for (auto const& a : albedo)
        lit.push_back(a * k_irradiance);

    auto const fill = [&](sg::command_list& cmd, sg::texture_2d const& t, tg::vec4f v)
    {
        auto const pixels = cc::vector<tg::vec4f>::create_filled(k_size * k_size, v);
        cmd.upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
    };

    auto history = sr::denoise_history();

    // Past REBLUR's `historyFixFrameNum` of 3, because that pass is a wide blur meant to carry a young history and
    // stops once there is one — measuring inside it would be measuring the transient rather than the member.
    // The session's own pipelines build after the first call reaches it, so the first calls report pending by design.
    auto denoised_frames = 0;
    for (auto attempt = 0; attempt < 16 && denoised_frames < 6; ++attempt)
    {
        auto cmd = ctx.create_command_list();

        cmd->upload.bytes_to_texture(diffuse.raw(), cc::span<tg::vec4f const>(lit).as_bytes());
        cmd->upload.bytes_to_texture(albedo_texture.raw(), cc::span<tg::vec4f const>(albedo).as_bytes());
        fill(*cmd, specular, tg::vec4f(k_specular_radiance, k_specular_radiance, k_specular_radiance, 0));
        fill(*cmd, specular_albedo, tg::vec4f(k_specular_albedo, k_specular_albedo, k_specular_albedo, 0));
        fill(*cmd, normal, tg::vec4f(0, 0, 1, 0));
        fill(*cmd, roughness, tg::vec4f(0.5f, 0, 0, 0));
        fill(*cmd, depth, tg::vec4f(5.0f, 0, 0, 0));
        fill(*cmd, motion, tg::vec4f(0, 0, 0, 0));
        {
            auto const pixels = cc::vector<tg::vec2f>::create_filled(k_size * k_size, tg::vec2f(2.0f, 2.0f));
            cmd->upload.bytes_to_texture(hit_distance.raw(), cc::span<tg::vec2f const>(pixels).as_bytes());
        }

        auto const in = sr::denoise_inputs{
            .color = diffuse,
            .specular = specular,
            .guides = {.albedo = albedo_texture,
                       .specular_albedo = specular_albedo,
                       .normal = normal,
                       .roughness = roughness,
                       .depth = depth,
                       .motion = motion,
                       .hit_distance = hit_distance},
            .output = output,
        };

        auto const outcome = sr::nrd_denoise_routine::execute(*cmd, in, history);
        REQUIRE(outcome.status != sr::denoise_status::unsupported);
        REQUIRE(outcome.status != sr::denoise_status::failed);
        if (outcome.is_denoised())
            ++denoised_frames;

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
    }

    REQUIRE(denoised_frames == 6).context("NRD never produced a denoised frame");

    auto cmd = ctx.create_command_list();
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_size * k_size);

    auto out = cc::vector<tg::vec4f>();
    out.reserve(pixels.size());
    for (auto i = isize(0); i < pixels.size(); ++i)
        out.push_back(pixels[i]);
    co_return out;
}
} // namespace

// The encodings, end to end, on a surface with nothing for REBLUR to blur.
//
// A uniform albedo means the de-modulated signal is uniform too, so whatever REBLUR does inside, the output has to
// come back as what went in — which makes this a check on every encoding step between us and NRD.
// The radiance is divided by a material factor, packed into YCoCg with a normalized hit distance in alpha, decoded,
// and multiplied back; a pack that dropped the chroma, a resolve that skipped the decode, or one that lost the factor
// all land on a different colour, and none of them would report an error.
ASYNC_INVOCABLE_TEST("sr - NRD returns a uniformly lit surface unchanged", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    if (!sr::query_denoise_support(ctx).nrd)
        SKIP("NRD was not fetched into this build (extern/nrd/fetch-nrd.py)");

    sr::nrd_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    // Channels that differ from one another, so a repack that collapsed colour onto luminance cannot pass.
    auto const albedo = tg::vec4f(0.85f, 0.7f, 0.2f, 0);
    auto const out = co_await denoise_lit_surface(ctx, cc::vector<tg::vec4f>::create_filled(k_size * k_size, albedo));

    for (auto const q : {tg::vec2i(16, 16), tg::vec2i(48, 16), tg::vec2i(48, 48)})
        for (auto c = 0; c < 3; ++c)
        {
            auto const expected = albedo[c] * k_irradiance + k_specular_radiance;
            auto const got = out[q[1] * k_size + q[0]][c];
            CHECK(tg::abs(got - expected) < 0.02f)
                .context(cc::format("pixel {},{} channel {}: got {}, expected {}", q[0], q[1], c, got, expected));
        }
}

// De-modulation, which is the difference between denoising a surface and smearing its texture.
//
// A checkerboard albedo under one uniform light is the case it exists for: nothing in the normal or the depth says
// there is an edge there, so REBLUR has every reason to average across it, and only dividing the albedo out keeps it
// from doing so.
//
// Asserted as contrast RETAINED rather than as an exact value, because de-modulation is partial by construction —
// NRD floors both factors well above zero and calls the specular half a biased solution — so some of the contrast
// does reach the blur.
// The bound has room on both sides rather than being fitted to what this machine returns: de-modulated, the three
// channels keep 0.85, 0.90 and 0.93 of the contrast; with the division and its inverse removed they keep 0.09, 0.09
// and 0.02, because REBLUR then sees the full seventeen-to-one radiance ratio and pulls the two cells together.
ASYNC_INVOCABLE_TEST("sr - NRD keeps a surface's texture rather than filtering it as noise",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    if (!sr::query_denoise_support(ctx).nrd)
        SKIP("NRD was not fetched into this build (extern/nrd/fetch-nrd.py)");

    sr::nrd_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const bright = tg::vec4f(0.85f, 0.7f, 0.2f, 0);
    auto const dark = tg::vec4f(0.05f, 0.1f, 0.6f, 0);

    auto albedo = cc::vector<tg::vec4f>();
    albedo.reserve(k_size * k_size);
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
            albedo.push_back((x / 32 + y / 32) % 2 == 0 ? bright : dark);

    auto const out = co_await denoise_lit_surface(ctx, cc::move(albedo));

    // Cell centres, as far from an edge as a two-by-two checker allows.
    auto const bright_out = out[16 * k_size + 16];
    auto const dark_out = out[16 * k_size + 48];

    for (auto c = 0; c < 3; ++c)
    {
        auto const in_contrast = (bright[c] - dark[c]) * k_irradiance;
        auto const out_contrast = bright_out[c] - dark_out[c];
        auto const retained = out_contrast / in_contrast;
        CHECK(retained > 0.6f)
            .context(cc::format("channel {}: kept {} of the input contrast ({} of {})", c, retained, out_contrast,
                                in_contrast));
    }
}

// The motion guide, which is the half of this integration that is ours and runs backwards from NRD's in both respects.
//
// sv writes pixels, current minus previous; NRD reads UV, previous minus current — `pixelUvPrev = pixelUv + IN_MV.xy *
// motionVectorScale.xy`. `nrd_session::execute` bridges both inversions on one line, by putting the reciprocal extent
// AND the sign on `motionVectorScale`, and hands the guide over untouched.
//
// A stream of zero motion cannot pin any of that: with every pixel reading its history from its own position, a
// flipped sign, a missing reciprocal or a motion texture bound to the wrong slot all converge just as well.
// So this translates a lit region across the image and tells NRD about it — or does not — and compares the two.
// It is the NRD counterpart of "sr - svgf follows a moving image through its motion vectors", and it measures the same
// way, because a wrong motion vector does not fail anything: it smears, and a soft image reads as a denoiser working.
namespace
{
/// Bigger than `k_size`: the region has to travel far enough that a history which did not follow it is blending
/// somewhere else entirely, and still leave both sides of the sweep clear of REBLUR's spatial reach.
constexpr auto k_motion_size = 128;

/// How far the lit edge moves per frame, in pixels, and how many frames of it are measured.
/// Eight frames of four pixels sweeps a 32-pixel band, every column of which was on both sides of the edge.
constexpr auto k_motion_step = 4;
constexpr auto k_motion_frames = 8;

constexpr auto k_bright = 1.0f;
constexpr auto k_dark = 0.2f;

/// A constant albedo, so the de-modulation factor is the same at every pixel and cannot itself carry the pattern.
constexpr auto k_motion_albedo = 0.5f;

/// The lighting at column `x` once the edge has reached `shift`: bright to its left, dark to its right.
///
/// The pattern is in the LIGHTING rather than in the albedo, and that is the whole reason this test can exist.
/// De-modulation divides the albedo out, so a moving albedo leaves REBLUR a uniform signal and nothing to reproject
/// wrongly; a moving irradiance is what actually reaches it.
[[nodiscard]] f32 lighting_at(int x, int shift)
{
    return x < k_motion_size / 4 + shift ? k_bright : k_dark;
}

/// Denoises `k_motion_frames` frames of that sweeping edge, telling NRD it moved by `told_step` pixels per frame, and
/// returns the mean absolute error of the last frame against the truth over the swept band.
///
/// `moving` false holds the edge still, which is the floor any run can reach.
[[nodiscard]] cc::shared_async<f32> sweep_error(sg::context& ctx, bool moving, int told_step)
{
    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_motion_size,
                                                 .height = k_motion_size,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const diffuse = make(sg::pixel_format::rgba32_float);
    auto const specular = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const roughness = make(sg::pixel_format::rgba32_float);
    auto const depth = make(sg::pixel_format::rgba32_float);
    auto const motion = make(sg::pixel_format::rgba32_float);
    auto const hit_distance = make(sg::pixel_format::rg32_float);
    auto const albedo_texture = make(sg::pixel_format::rgba32_float);
    auto const specular_albedo = make(sg::pixel_format::rgba32_float);
    auto const output = make(sg::pixel_format::rgba32_float);

    auto const fill = [&](sg::command_list& cmd, sg::texture_2d const& t, tg::vec4f v)
    {
        auto const pixels = cc::vector<tg::vec4f>::create_filled(k_motion_size * k_motion_size, v);
        cmd.upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
    };

    auto history = sr::denoise_history();

    auto denoised_frames = 0;
    for (auto attempt = 0; attempt < 24 && denoised_frames < k_motion_frames; ++attempt)
    {
        auto cmd = ctx.create_command_list();

        // The shift counts DENOISED frames rather than attempts: the first calls report pending while the pipelines
        // build, and a pattern that moved during them would be telling NRD about motion it never saw.
        auto const shift = moving ? denoised_frames * k_motion_step : 0;
        {
            auto pixels = cc::vector<tg::vec4f>();
            pixels.reserve(k_motion_size * k_motion_size);
            for (auto y = 0; y < k_motion_size; ++y)
                for (auto x = 0; x < k_motion_size; ++x)
                {
                    auto const lit = k_motion_albedo * lighting_at(x, shift);
                    pixels.push_back(tg::vec4f(lit, lit, lit, 0));
                }
            cmd->upload.bytes_to_texture(diffuse.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
        }

        fill(*cmd, albedo_texture, tg::vec4f(k_motion_albedo, k_motion_albedo, k_motion_albedo, 0));

        // No specular at all: a zero F0 floors its factor, so the specular half contributes nothing and what comes out
        // is the diffuse lobe alone — which is the one whose reprojection this is about.
        fill(*cmd, specular, tg::vec4f(0, 0, 0, 0));
        fill(*cmd, specular_albedo, tg::vec4f(0, 0, 0, 0));
        fill(*cmd, normal, tg::vec4f(0, 0, 1, 0));
        fill(*cmd, roughness, tg::vec4f(0.5f, 0, 0, 0));
        fill(*cmd, depth, tg::vec4f(5.0f, 0, 0, 0));

        // Our convention: this frame's pixel minus last frame's, in pixels.
        // A region that moved +4 in x carries (4, 0).
        fill(*cmd, motion, tg::vec4f(f32(told_step), 0, 0, 0));
        {
            auto const pixels
                = cc::vector<tg::vec2f>::create_filled(k_motion_size * k_motion_size, tg::vec2f(2.0f, 2.0f));
            cmd->upload.bytes_to_texture(hit_distance.raw(), cc::span<tg::vec2f const>(pixels).as_bytes());
        }

        auto const in = sr::denoise_inputs{
            .color = diffuse,
            .specular = specular,
            .guides = {.albedo = albedo_texture,
                       .specular_albedo = specular_albedo,
                       .normal = normal,
                       .roughness = roughness,
                       .depth = depth,
                       .motion = motion,
                       .hit_distance = hit_distance},
            .output = output,
        };

        auto const outcome = sr::nrd_denoise_routine::execute(*cmd, in, history);
        REQUIRE(outcome.status != sr::denoise_status::unsupported);
        REQUIRE(outcome.status != sr::denoise_status::failed);
        if (outcome.is_denoised())
            ++denoised_frames;

        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
    }

    REQUIRE(denoised_frames == k_motion_frames).context("NRD never produced enough denoised frames");

    auto cmd = ctx.create_command_list();
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_motion_size * k_motion_size);

    // Measured over the band the edge swept, and only there: outside it every run agrees, so including it would
    // dilute the difference this test exists to see.
    auto const final_shift = moving ? (k_motion_frames - 1) * k_motion_step : 0;
    auto error = 0.0f;
    auto count = 0;
    for (auto y = k_motion_size / 4; y < 3 * k_motion_size / 4; ++y)
        for (auto x = k_motion_size / 4; x < k_motion_size / 4 + k_motion_frames * k_motion_step; ++x)
        {
            auto const expected = k_motion_albedo * lighting_at(x, final_shift);
            error += tg::abs(pixels[y * k_motion_size + x][0] - expected);
            ++count;
        }
    REQUIRE(count > 0);
    co_return error / f32(count);
}
} // namespace

// Three runs of the same sweeping edge: told the truth, told nothing moved, and never moving.
//
// The middle one is the control, and the comparison between the first two is the assertion: a history that did not
// follow the edge is blending the bright side into the dark one across the whole swept band.
// The third is the floor — what this measurement reads when there is no reprojection to get wrong — and it is what
// keeps "told the truth" from passing on a run that simply failed to accumulate anything.
ASYNC_INVOCABLE_TEST("sr - NRD follows a moving image through its motion vectors", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    if (!sr::query_denoise_support(ctx).nrd)
        SKIP("NRD was not fetched into this build (extern/nrd/fetch-nrd.py)");

    sr::nrd_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const with_motion = co_await sweep_error(ctx, true, k_motion_step);
    auto const without_motion = co_await sweep_error(ctx, true, 0);
    auto const never_moved = co_await sweep_error(ctx, false, 0);

    // Measured AGAINST THE FLOOR rather than against zero, which is what the numbers made necessary.
    //
    // REBLUR blurs a step edge spatially whatever the history does, so most of every figure here is that blur and not
    // reprojection: measured on this machine, 0.130 / 0.180 / 0.129 for the three runs.
    // A plain ratio of the first two is therefore about 0.72 however perfectly the motion is followed, and a threshold
    // under it would be pinning REBLUR's blur radius rather than our motion conversion.
    // Subtracting the never-moved run removes exactly the part neither run can avoid, and what is left is the
    // reprojection error alone — 0.0007 against 0.0505, a factor of seventy.
    auto const gap = without_motion - never_moved;

    // Vacuous otherwise: if telling NRD nothing moved cost nothing, there would be no mechanism under test, and the
    // check below would pass on any pair of numbers.
    REQUIRE(gap > 0.2f * never_moved)
        .context(cc::format("a wrong motion vector cost almost nothing — with {}, without {}, never moved {}",
                            with_motion, without_motion, never_moved));

    // A quarter of that gap rather than something tighter: the defects this exists to catch — a flipped sign, a
    // missing reciprocal extent, a motion texture bound to the wrong slot — do not shave the number, they remove
    // reprojection, so the result lands at or above the told-nothing-moved figure.
    // The slack is what keeps a float-rounding difference between two drivers from failing a factor-of-seventy result.
    CHECK(with_motion - never_moved < 0.25f * gap)
        .context(cc::format("error with motion {}, without {}, never moved {}", with_motion, without_motion, never_moved));
}

// Moving a history that owns a vendor object, which is the reason `denoise_history` stopped being defaulted.
//
// The slot pairs a `void*` with the function that frees it, and losing it is silent in every way a status flag can see.
// `_prepare` decides `restarted` from the method and the extent alone.
// So a history that arrived at a move with no session still reports a continuing stream, builds a fresh NRD instance
// out of the context's already-warm pipeline cache, and denoises the very next frame.
// A first draft of this test asserted on `restarted`, and passed with the move constructor deliberately dropping the slot.
//
// What an NRD instance actually holds is REBLUR's accumulated history, so the one observable difference is temporal
// LAG: a stream fed several bright frames and then a dark one comes back part-way between, while a stream whose
// instance was rebuilt has nothing to blend and lands near the dark value.
// So this runs that sequence twice, moving the history in one of them, and requires the two to agree.
namespace
{
constexpr auto k_move_bright = 0.8f;
constexpr auto k_move_dark = 0.1f;

/// Feeds `k_move_warmup` bright frames and then one dark frame, and returns the centre pixel's red channel.
///
/// `move_between` moves the history -- by construction and then by assignment over a live one -- after the bright
/// frames and before the dark one, which is the only difference between the two runs.
[[nodiscard]] cc::shared_async<f32> lag_after_darkening(sg::context& ctx, bool move_between)
{
    constexpr auto k_move_warmup = 6;

    auto const make = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = k_size,
                                                 .height = k_size,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image
                                                        | sg::texture_usage::copy_dst | sg::texture_usage::copy_src});
    };

    auto const diffuse = make(sg::pixel_format::rgba32_float);
    auto const specular = make(sg::pixel_format::rgba32_float);
    auto const normal = make(sg::pixel_format::rgba32_float);
    auto const roughness = make(sg::pixel_format::rgba32_float);
    auto const depth = make(sg::pixel_format::rgba32_float);
    auto const motion = make(sg::pixel_format::rgba32_float);
    auto const hit_distance = make(sg::pixel_format::rg32_float);
    auto const albedo_texture = make(sg::pixel_format::rgba32_float);
    auto const specular_albedo = make(sg::pixel_format::rgba32_float);
    auto const output = make(sg::pixel_format::rgba32_float);

    auto const in = sr::denoise_inputs{
        .color = diffuse,
        .specular = specular,
        .guides = {.albedo = albedo_texture,
                   .specular_albedo = specular_albedo,
                   .normal = normal,
                   .roughness = roughness,
                   .depth = depth,
                   .motion = motion,
                   .hit_distance = hit_distance},
        .output = output,
    };

    auto const fill = [&](sg::command_list& cmd, sg::texture_2d const& t, tg::vec4f v)
    {
        auto const pixels = cc::vector<tg::vec4f>::create_filled(k_size * k_size, v);
        cmd.upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
    };

    // One frame at irradiance `lit`, against whichever history it is handed.
    auto const run_one = [&](sr::denoise_history& history, f32 lit) -> cc::shared_async<sr::denoise_outcome>
    {
        auto cmd = ctx.create_command_list();

        // A constant albedo, so the de-modulated signal is the irradiance and the step below reaches REBLUR intact.
        constexpr auto k_move_albedo = 0.5f;
        fill(*cmd, diffuse, tg::vec4f(k_move_albedo * lit, k_move_albedo * lit, k_move_albedo * lit, 0));
        fill(*cmd, albedo_texture, tg::vec4f(k_move_albedo, k_move_albedo, k_move_albedo, 0));
        fill(*cmd, specular, tg::vec4f(0, 0, 0, 0));
        fill(*cmd, specular_albedo, tg::vec4f(0, 0, 0, 0));
        fill(*cmd, normal, tg::vec4f(0, 0, 1, 0));
        fill(*cmd, roughness, tg::vec4f(0.5f, 0, 0, 0));
        fill(*cmd, depth, tg::vec4f(5.0f, 0, 0, 0));
        fill(*cmd, motion, tg::vec4f(0, 0, 0, 0));
        {
            auto const pixels = cc::vector<tg::vec2f>::create_filled(k_size * k_size, tg::vec2f(2.0f, 2.0f));
            cmd->upload.bytes_to_texture(hit_distance.raw(), cc::span<tg::vec2f const>(pixels).as_bytes());
        }

        auto const outcome = sr::nrd_denoise_routine::execute(*cmd, in, history);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();

        cc::async_backlog const* const backlogs[] = {&ctx.backlog};
        co_await cc::async_settled(cc::async_backlog::settled(backlogs));
        co_return outcome;
    };

    auto history = sr::denoise_history();
    auto denoised = 0;
    for (auto attempt = 0; attempt < 24 && denoised < k_move_warmup; ++attempt)
    {
        auto const outcome = co_await run_one(history, k_move_bright);
        REQUIRE(outcome.status != sr::denoise_status::failed);
        if (outcome.is_denoised())
            ++denoised;
    }
    REQUIRE(denoised == k_move_warmup).context("the bright stream never got going");

    // Both move operations, on a history whose NRD instance is now carrying six frames of bright.
    auto moved = sr::denoise_history();
    if (move_between)
    {
        auto intermediate = cc::move(history); // move construction
        moved = cc::move(intermediate);        // move assignment, over a history that owns nothing yet
    }
    auto& live = move_between ? moved : history;

    {
        auto const outcome = co_await run_one(live, k_move_dark);
        REQUIRE(outcome.is_denoised());
    }

    auto cmd = ctx.create_command_list();
    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(output.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    REQUIRE(pixels.size() == k_size * k_size);
    co_return pixels[(k_size / 2) * k_size + k_size / 2][0];
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - a denoise history carries its vendor state through a move", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary
    if (!sr::query_denoise_support(ctx).nrd)
        SKIP("NRD was not fetched into this build (extern/nrd/fetch-nrd.py)");

    sr::nrd_denoise_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const kept = co_await lag_after_darkening(ctx, false);
    auto const moved = co_await lag_after_darkening(ctx, true);

    // The darkened frame has to land in the same place either way: the move must carry REBLUR's accumulated history,
    // not just the textures around it.
    //
    // Measured as a fraction of the step rather than as an absolute, so the bound says what it means -- the two runs
    // must agree to within a twentieth of the distance the image travelled.
    auto const step = tg::abs(kept - 0.5f * k_move_dark);
    CHECK(tg::abs(moved - kept) < 0.05f * cc::max(step, 0.01f)).context(cc::format("kept {}, moved {}", kept, moved));

    // And the measurement must have something to see: a run showing no lag at all would make the check above pass on
    // two identical failures.
    // Half the albedo is what an unaccumulated dark frame reads, so a lagging one sits well above it.
    CHECK(kept > 0.5f * k_move_dark * 1.5f).context(cc::format("no temporal lag to detect: {}", kept));
}

#endif
