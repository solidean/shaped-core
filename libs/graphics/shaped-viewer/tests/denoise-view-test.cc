#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// A denoised layer, end to end through the headless viewer.
//
// What this pins is the promise `render_settings::denoise` makes: turning denoising on, off or to another member never
// restarts accumulation.
// The guides and the denoised image are written beside the mean, never into it, so the count a capture waits on keeps
// climbing across every change — while the denoiser's own guides restart on a count of their own.
ASYNC_INVOCABLE_TEST("sv - denoising a layer never restarts its accumulation", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the path-tracing shaders");

    auto const size = tg::vec2i(96, 64); // small: WARP traces every pixel in software
    auto v_r = sv::viewer::try_create(ctx, "sv-test/denoise", {.width = size[0], .height = size[1], .headless = true});
    REQUIRE(v_r.has_value());
    auto viewer = cc::move(v_r.value());

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);

    // Off, then on, then another member, then off again — each phase long enough for a trace to land in it.
    auto const phase_methods = cc::vector<sr::denoise_method>{
        sr::denoise_method::none,
        sr::denoise_method::atrous,
        sr::denoise_method::automatic,
        sr::denoise_method::none,
    };
    constexpr u32 frames_per_phase = 3;

    auto phase = isize(0);
    auto phase_start = u32(0);
    auto last_accumulated = u32(0);
    auto never_dropped = true;
    auto first_drop = cc::string();
    auto history = cc::string(); // every frame's (phase, count), so a failure shows the whole sequence
    auto was_resident = false;

    // WORKAROUND, and the same one sv_test::tick_until carries: a trace declines until its permutations and the
    // denoiser's shader have compiled, and those compile on the ambient scheduler, so the guard is a deadline.
    //
    // It must stay well under dev.py's per-binary timeout: a deadline above it turns a stall into a killed process
    // whose only evidence is a stack, where this reports which phase it was in and what was still streaming.
    auto const loop_start = cc::current_time_steady_secs();

    for (auto f : viewer.frames())
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 6.0});

        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        scene.add_rect_light("key", tg::pos3f(0, 1.9f, 0), tg::vec3f(0.4f, 0, 0), tg::vec3f(0, 0, 0.4f)).nits(12);
        // The loop may run one more body after the close request; it then keeps the last phase's setting.
        auto const method = phase < phase_methods.size() ? phase_methods[phase] : phase_methods.back();
        scene.settings({.samples_per_pixel = 1, .denoise = {.method = method}});

        auto const accumulated = view.accumulated_frames();
        // Bounded: a loop that never converges runs thousands of frames, and the first few hundred say what went wrong.
        if (history.size() < 2000)
            history += cc::format(" {}:{}", phase, accumulated);
        if (accumulated < last_accumulated && never_dropped)
        {
            never_dropped = false;
            first_drop = cc::format("from {} to {} in phase {}", last_accumulated, accumulated, phase);
        }
        last_accumulated = accumulated;

        // Nothing counts until the box has streamed in: its landing restarts the accumulation, and legitimately, since the
        // image stops being a picture of the placeholder — which is a restart this test is not about.
        // The first resident frame still reads the placeholder's count, since the count lags a frame, so it waits too.
        auto const resident = f.streaming_resources() == 0 && f.pending_resource_work() == 0;
        auto const counting = resident && was_resident;
        was_resident = resident;
        if (!counting)
        {
            last_accumulated = 0;
            phase_start = 0;
            if (history.size() < 2000)
                history += " (streaming)";
        }

        // A phase advances on frames that traced, not on frames: before the first trace lands nothing is being tested.
        else if (accumulated >= phase_start + frames_per_phase)
        {
            ++phase;
            phase_start = accumulated;
            if (phase == phase_methods.size())
                viewer.request_close();
        }
        REQUIRE(cc::current_time_steady_secs() - loop_start < 25.0)
            .context(cc::format("phase {}, accumulated {}, streaming {}, pending {}; frames (phase:count):{}", phase,
                                accumulated, f.streaming_resources(), f.pending_resource_work(), history));
    }

    co_await cc::async_settled(sv::background_work(ctx));

    CHECK(phase == phase_methods.size());
    CHECK(never_dropped).context(cc::format("{}; frames (phase:count):{}", first_drop, history));
    CHECK(last_accumulated >= frames_per_phase * u32(phase_methods.size()));
}

namespace
{
/// Captures the cornell box at `accumulate` frames of one sample each, denoised by `method`, as a PNG at `path`.
/// PNG rather than the JPEG default, because JPEG's own smoothing would blur exactly the difference under test.
cc::shared_async<cc::unit> capture_box(sg::context& ctx, sr::denoise_method method, cc::string const& path)
{
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, "96x64");
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "2");
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "20");

    auto const box = sv_test::make_cornell_box();
    auto const mesh = sv_test::as_mesh("cornell box", box.positions, box.materials);
    auto const loop_start = cc::current_time_steady_secs();

    for (auto f : sv::interactive(ctx, "sv-test/denoise-capture"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = tg::pos3d(0, 0, 0), .distance = 6.0});
        f.register_capture("front", [](sv::capture_context const&) {});

        auto scene = view.add_scene();
        scene.add_mesh(mesh);
        // Just under the ceiling rather than above it as the other viewer tests place it: only the analytic light is
        // sampled directly, so from above the box is lit through its lamp mesh alone and comes out nearly black.
        // Moderate, because nothing tone-maps and a bright box clips to flat white; either way the noise would hide.
        scene.add_rect_light("key", tg::pos3f(0, 0.99f, 0), tg::vec3f(0.35f, 0, 0), tg::vec3f(0, 0, 0.35f)).nits(4);
        scene.settings({.samples_per_pixel = 1, .denoise = {.method = method}});

        // The capture ends the loop itself; the deadline only turns a hang into a message.
        // Under dev.py's per-binary timeout, so a stall reports rather than being killed — as is the capture's own above.
        REQUIRE(cc::current_time_steady_secs() - loop_start < 25.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

/// The mean absolute difference between horizontally adjacent pixels, over every channel — what noise inflates and a
/// denoiser should bring down.
/// Pairs touching 0 or 255 are skipped: nothing tone-maps the HDR mean, so a clipped region is flat whatever the noise.
[[nodiscard]] f64 roughness_of(cc::string const& path)
{
    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto const decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());

    auto const& img = decoded.value();
    REQUIRE(img.comp == babel::image::component::u8);
    auto sum = 0.0;
    auto count = 0;
    auto clipped = 0;
    for (auto y = 0; y < img.height; ++y)
        for (auto x = 0; x + 1 < img.width; ++x)
            for (auto c = 0; c < img.channels; ++c)
            {
                auto const a = int(img.pixels[(y * img.width + x) * img.channels + c]);
                auto const b = int(img.pixels[(y * img.width + x + 1) * img.channels + c]);
                if (a == 0 || a == 255 || b == 0 || b == 255)
                {
                    ++clipped;
                    continue;
                }
                sum += tg::abs(a - b);
                ++count;
            }
    // An image with nothing but clipped pixels says nothing about noise, and reading it as perfectly smooth would pass.
    REQUIRE(count > 0).context(cc::format("{}: all {} neighbour pairs clipped", path, clipped));
    return sum / count;
}
} // namespace

// The one check that the viewer PRESENTS the denoised image rather than only computing it.
// Captures of the same two-frame estimate: each denoised one must be clearly smoother than the raw one.
//
// Two denoised ones, because a two-frame mean is young: à-trous, named, runs spatially on the mean, while `automatic`
// takes the temporal branch and runs SVGF on the frame's own samples with the motion guide.
// So the second capture is what pins the raygen's per-frame target and motion vectors reaching a temporal member.
// The capture protocol is process environment, which is why the drivers in dx12-entry.cc carry capture-environment.
ASYNC_INVOCABLE_TEST("sv - a denoised capture is smoother than the raw one", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    if (!sv_test::shared_env().has_compiler)
        SKIP("no DXC compiler to build the path-tracing shaders");

    auto const raw_path = cc::format("{}/sv-denoise-raw.png", cc::temp_directory_path());
    auto const spatial_path = cc::format("{}/sv-denoise-atrous.png", cc::temp_directory_path());
    auto const temporal_path = cc::format("{}/sv-denoise-automatic.png", cc::temp_directory_path());

    co_await capture_box(ctx, sr::denoise_method::none, raw_path);
    co_await capture_box(ctx, sr::denoise_method::atrous, spatial_path);
    co_await capture_box(ctx, sr::denoise_method::automatic, temporal_path);

    auto const raw = roughness_of(raw_path);
    auto const spatial = roughness_of(spatial_path);
    auto const temporal = roughness_of(temporal_path);
    CHECK(spatial < 0.7 * raw).context(cc::format("roughness raw {} atrous {}", raw, spatial));
    CHECK(temporal < 0.7 * raw).context(cc::format("roughness raw {} automatic (svgf) {}", raw, temporal));

    cc::remove_file(raw_path);
    cc::remove_file(spatial_path);
    cc::remove_file(temporal_path);
}
