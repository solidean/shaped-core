#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/common/macros.hh> // CC_ARCH_ARM64
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/record/recording.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/rec.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// The wavefront tracer against the megakernel through the whole viewer, on what wavefront-trace-test.cc does not reach:
// quadrics, which the wavefront intersects in its extend stage and shades with their own kernels, and decals painted at hits.
//
// Both captures are means of the same samples per pixel, compared block by block, so what is left between them is Monte Carlo noise.

namespace
{
constexpr auto k_size = tg::vec2i(96, 64);
constexpr int k_block = 8;

/// The Cornell box with three spheres and a decal on its back wall, built once and placed into a view every frame.
struct parity_scene
{
    sv_test::lit_cornell_box lit;
    sv::drawing square;
    sv::camera camera;

    [[nodiscard]] static parity_scene create()
    {
        auto const box = sv_test::make_cornell_box();
        // Lit by its own lamp, an emissive mesh, which is the box's only light: both tracers sample it.
        auto out = parity_scene{.lit = sv_test::as_lit_meshes(box, 4.0f),
                                .square = sv::drawing(),
                                .camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)}};
        out.square.add_fill(sv::path::rectangle(tg::aabb2f(tg::pos2f(-0.3f, -0.3f), tg::pos2f(0.3f, 0.3f))),
                            {.color = tg::vec4f(1, 0, 0, 1)});
        out.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
        return out;
    }

    /// Places the scene in `view`, traced in `mode` under `frame_budget_ms`.
    void place(sv::view_ref view, sv::render_mode mode, f32 frame_budget_ms) const
    {
        view.camera(camera);
        auto scene = view.add_scene();
        scene.add_mesh(lit.enclosure);
        scene.add_mesh(lit.lamp);
        scene.add_sphere(tg::sphere3f(tg::pos3f(-0.45f, -0.65f, 0.2f), 0.35f));
        scene.add_sphere(tg::sphere3f(tg::pos3f(0.45f, -0.7f, -0.2f), 0.3f));
        scene.add_sphere(tg::sphere3f(tg::pos3f(0.0f, 0.3f, 0.5f), 0.2f));
        // On the back wall, read from the camera: x to the right, y down, so it projects away from the reader into the wall.
        scene.add_decal(square, {.at = tg::pos3f(0.45f, 0.35f, 1.0f),
                                 .x_axis = tg::vec3f(1, 0, 0),
                                 .y_axis = tg::vec3f(0, -1, 0),
                                 .scale = 1,
                                 .depth = 0.5f});
        scene.settings({.samples_per_pixel = 4, .mode = mode, .frame_budget_ms = frame_budget_ms});
    }
};

/// Captures the parity scene traced in `mode` at `accumulate` samples per pixel.
/// PNG, so nothing but the tracer's own noise separates two captures.
cc::shared_async<cc::unit> capture_scene(sg::context& ctx,
                                         sv::render_mode mode,
                                         cc::string const& path,
                                         int accumulate,
                                         f32 frame_budget_ms)
{
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", k_size[0], k_size[1]));
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, cc::format("{}", accumulate));
    // A capture that runs first in its process compiles every wavefront kernel cold, which an unoptimized build's SGL
    // compiler takes about a minute for, before the first of its frames.
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "120");

    auto const scene = parity_scene::create();
    auto const loop_start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/wavefront-parity"))
    {
        f.register_capture("front", [](sv::capture_context const&) {});
        scene.place(f.window().view(), mode, frame_budget_ms);

        REQUIRE(cc::current_time_steady_secs() - loop_start < 110.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));
}

/// The PNG at `path`, as its 8-bit samples.
[[nodiscard]] babel::image::image read_png(cc::string const& path)
{
    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().comp == babel::image::component::u8);
    REQUIRE(decoded.value().width == k_size[0]);
    REQUIRE(decoded.value().height == k_size[1]);
    return cc::move(decoded.value());
}

/// The mean of channel `c` over block (`bx`, `by`), in [0, 255].
[[nodiscard]] f64 block_mean(babel::image::image const& img, int bx, int by, int c)
{
    auto sum = 0.0;
    for (auto y = by * k_block; y < (by + 1) * k_block; ++y)
        for (auto x = bx * k_block; x < (bx + 1) * k_block; ++x)
            sum += f64(img.pixels[(y * img.width + x) * img.channels + c]);
    return sum / f64(k_block * k_block);
}

/// The least value of every stat snapshot named `name` in `r`, or 2 when there is none.
[[nodiscard]] f64 least_stat(cc::rec::recording const& r, cc::string_view name)
{
    auto least = 2.0;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() == cc::rec::event_kind::stat_snapshot && cc::string_view(e.name()) == name)
                least = cc::min(least, e.field_as_double("value").value_or(2.0));
        });
    return least;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - the wavefront traces the megakernel's quadrics and decals", (sg::context_handle const& ctx_h))
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
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("the wavefront tracer needs inline ray queries");

    auto const reference_path = cc::format("{}/sv-parity-megakernel.png", cc::temp_directory_path());
    auto const traced_path = cc::format("{}/sv-parity-wavefront.png", cc::temp_directory_path());

    // The same samples per pixel for both, so the noise left between them is the same: 48 megakernel frames of 4.
    co_await capture_scene(ctx, sv::render_mode::megakernel, reference_path, 192, 0.0f);
    co_await capture_scene(ctx, sv::render_mode::wavefront, traced_path, 192, 0.0f);

    auto const reference = read_png(reference_path);
    auto const traced = read_png(traced_path);

    // Every channel, so the decal's red is compared as well as the light: a wall left gray where the megakernel paints it red
    // is a block whose red and green both move.
    auto worst = 0.0;
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
            for (auto c = 0; c < 3; ++c)
            {
                auto const r = block_mean(reference, bx, by, c);
                auto const w = block_mean(traced, bx, by, c);
                auto const err = tg::abs(w - r) / (r + 12.0);
                worst = cc::max(worst, err);
                CHECK(err < 0.25)
                    .context(cc::format("block ({}, {}) channel {}: wavefront {}, megakernel {}", bx, by, c, w, r));
            }
    CC_LOG_INFO("wavefront against megakernel through the viewer: worst block {}", worst);

    cc::remove_file(reference_path);
    cc::remove_file(traced_path);
}

// A still frame whose budget no round fits runs one round, starting samples in only a share of the free slots.
// wavefront-trace-test.cc pins that a partial fill keeps the image's mean; this pins that the viewer lowers it at all.
ASYNC_INVOCABLE_TEST("sv - a still wavefront view held to a budget no round fits lowers its fill",
                     (sg::context_handle const& ctx_h),
                     nx::config::recorded)
{
#if defined(CC_ARCH_ARM64) && defined(_WIN32)
    SKIP("known broken on Windows on ARM — the inline readback path fastfails; see "
         "libs/graphics/shaped-viewer/docs/TODO.md");
#endif
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the path-tracing shaders");
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("the wavefront tracer needs inline ray queries");
    if (!ctx.supports(sg::feature::timestamp_query))
        SKIP("a frame budget is paced by GPU timestamps, which this device does not take");

    auto rec = nx::test_recording();
    REQUIRE(rec.is_attached());

    // A loop of its own rather than a capture, which would also wait out the loading under its own clock.
    // At the fill this budget ends at, the slowest pixel's second sample is hundreds of frames away.
    auto v_r = sv::viewer::try_create(ctx, "sv-test/starved-wavefront",
                                      {.width = k_size[0], .height = k_size[1], .headless = true});
    REQUIRE(v_r.has_value());
    auto viewer = cc::move(v_r.value());

    // A microsecond: no round of any scene fits, so every measured frame lowers the fill.
    auto const scene = parity_scene::create();
    constexpr int still_frames = 48;
    auto traced_frames = 0;
    auto samples = u32(0);
    auto const loop_start = cc::current_time_steady_secs();
    for (auto f : viewer.frames())
    {
        auto view = f.window().view();
        scene.place(view, sv::render_mode::wavefront, 0.001f);

        // Counted from the first frame whose samples read back, since a trace declines until its kernels compiled.
        // Past the restart's one sample, so the starved still frames are what raised the floor.
        samples = view.accumulated_samples();
        if (samples > 0)
            ++traced_frames;
        if (traced_frames >= still_frames && samples >= 2)
            viewer.request_close();
        REQUIRE(cc::current_time_steady_secs() - loop_start < 110.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    auto const fill = least_stat(rec.sync(), "sv.wavefront.still_fill");
    CHECK(fill < 0.5).context(cc::format("the least fill a still frame ran at: {}", fill));

    // Fewer samples per frame, but samples: every pixel holds one past the restart's.
    CHECK(samples >= 2).context(cc::format("the floor at the end: {}", samples));
}
