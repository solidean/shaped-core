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

    /// Places the scene in `view`, traced in `mode`.
    void place(sv::view_ref view, sv::render_mode mode) const
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
        scene.settings({.samples_per_pixel = 4, .mode = mode});
    }
};

/// Captures the parity scene traced in `mode` at `accumulate` samples per pixel.
/// PNG, so nothing but the tracer's own noise separates two captures.
cc::shared_async<cc::unit> capture_scene(sg::context& ctx, sv::render_mode mode, cc::string const& path, int accumulate)
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
        scene.place(f.window().view(), mode);

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

} // namespace

ASYNC_INVOCABLE_TEST("sv - the wavefront traces the megakernel's quadrics and decals", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    // TEMPORARY(bug-sv-warp-megakernel-quadric-flake): the megakernel's quadrics come out wrong on CI's WARP under load.
    if (ctx.metrics.adapter().is_software)
        SKIP("flaky on WARP, where the megakernel's quadrics come out wrong under load; "
             "issue: bug-sv-warp-megakernel-quadric-flake");

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler for this backend to build the path-tracing shaders");
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        SKIP("the wavefront tracer needs inline ray queries");

    auto const reference_path = cc::format("{}/sv-parity-megakernel.png", cc::temp_directory_path());
    auto const traced_path = cc::format("{}/sv-parity-wavefront.png", cc::temp_directory_path());

    // The same samples per pixel for both, so the noise left between them is the same: 48 megakernel frames of 4.
    co_await capture_scene(ctx, sv::render_mode::megakernel, reference_path, 192);
    co_await capture_scene(ctx, sv::render_mode::wavefront, traced_path, 192);

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
