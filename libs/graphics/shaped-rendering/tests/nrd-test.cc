#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include "shader_fixtures.hh"

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
        return ctx.persistent.create_texture_2d({.format = format,
                                                 .width = extent[0],
                                                 .height = extent[1],
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
                                                        | sg::texture_usage::copy_dst});
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
                                                 .usage = sg::texture_usage::readonly_texture
                                                        | sg::texture_usage::readwrite_texture
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
ASYNC_INVOCABLE_TEST("sr - NRD returns a uniformly lit surface unchanged",
                     (sg::context_handle const& ctx_h))
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
#endif
