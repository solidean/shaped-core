#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// Bloom on a layer: what `settings.display.bloom` asks for reaches the screen, and asking for it never restarts the trace.
//
// A black quad in front of a bright sky is the scene that tells them apart: with nothing of its own, whatever a pixel of the quad shows came from the sky.

namespace
{
using namespace cc::primitive_defines;

constexpr auto k_size = tg::vec2i(64, 48);

[[nodiscard]] sv::mesh black_quad(sg::context& ctx)
{
    (void)ctx;
    auto& lib = sv_test::shared_material_library();
    auto const black
        = lib.acquire(sv::material::create("bloom black", lib.acquire_type(sv::builtin_material::openpbr).value(),
                                           {sv::material_attribute_binding::of("base_color", tg::vec3f(0, 0, 0)),
                                            sv::material_attribute_binding::of("specular_weight", 0.0f)}));
    auto const e = 0.7f;
    return sv::mesh{.name = "bloom quad",
                    .geometry = sv::triangle_geometry::create_from_positions(
                        cc::vector<tg::pos3f>{tg::pos3f(-e, -e, 0), tg::pos3f(e, e, 0), tg::pos3f(e, -e, 0),
                                              tg::pos3f(-e, -e, 0), tg::pos3f(-e, e, 0), tg::pos3f(e, e, 0)}),
                    .material = black};
}

/// The red value in the middle of the quad, in a capture under `settings`, which `each_frame` sees before every frame.
template <class FrameF>
cc::shared_async<f32> captured_middle(sg::context& ctx,
                                      sr::display_transform display,
                                      cc::string_view samples,
                                      FrameF each_frame)
{
    auto const path = cc::format("{}/sv-bloom-view.png", cc::temp_directory_path());
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", k_size[0], k_size[1]));
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, samples);
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "60");

    auto const quad = black_quad(ctx);
    for (auto f : sv::interactive(ctx, "sv-test/bloom-view"))
    {
        auto view = f.window().view();
        view.camera(sv::camera::looking_at(tg::pos3d(0, 0, -3), tg::pos3d(0, 0, 0)));
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(quad);
        scene.background(sv::background::uniform(tg::vec3f(8.0f, 8.0f, 8.0f)));
        scene.fallback_light(cc::nullopt);
        auto shown = display;
        each_frame(view, shown);
        scene.settings({.mode = sv::render_mode::megakernel, .display = shown});
    }
    co_await cc::async_settled(sv::background_work(ctx));

    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto const decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());
    auto const& img = decoded.value();
    auto const middle = (k_size[1] / 2 * img.width + k_size[0] / 2) * img.channels;
    auto const value = f32(img.pixels[middle]);
    cc::remove_file(path);
    co_return value;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a layer's bloom spreads bright light onto what stands in front of it, and only when asked",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    auto const nothing = [](sv::view_ref const&, sr::display_transform&) {};
    auto const plain = co_await captured_middle(ctx, {.curve = sr::display_curve::neutral}, "16", nothing);
    auto const bloomed = co_await captured_middle(
        ctx, {.curve = sr::display_curve::neutral, .bloom = {.strength = 0.5f}}, "16", nothing);

    CHECK(plain <= 1.0f).dump("plain", plain);
    CHECK(bloomed >= plain + 30.0f).dump("plain", plain).dump("bloomed", bloomed);
}

ASYNC_INVOCABLE_TEST("sv - turning a layer's bloom on and off keeps its accumulated samples",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    // From past the start only: a view restarts on purpose while its material compiles, which has nothing to do with bloom.
    constexpr auto settled = u32(32);

    auto highest = u32(0);
    auto frames = 0;
    (void)co_await captured_middle(ctx, {.curve = sr::display_curve::neutral}, "256",
                                   [&](sv::view_ref const& view, sr::display_transform& shown)
                                   {
                                       auto const samples = view.accumulated_samples();
                                       if (samples < settled && highest < settled)
                                       {
                                           highest = samples;
                                           return;
                                       }
                                       CHECK(samples >= highest).dump("frame", frames);
                                       highest = cc::max(highest, samples);
                                       if ((frames++ / 4) % 2 == 0)
                                           shown.bloom.strength = 0.5f;
                                   });
    CHECK(frames > 8);
}

ASYNC_INVOCABLE_TEST("sv - a layer's grade changes what it shows, and a neutral one changes nothing",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    auto const nothing = [](sv::view_ref const&, sr::display_transform&) {};
    auto const base = sr::display_transform{.exposure = -5.0f,
                                            .curve = sr::display_curve::neutral,
                                            .bloom = {.strength = 0.5f, .knee = 0.0f}};
    auto const plain = co_await captured_middle(ctx, base, "16", nothing);
    auto warm = base;
    warm.grade.temperature = 0.5f;
    auto const warmer = co_await captured_middle(ctx, warm, "16", nothing);
    CHECK(warmer > plain + 5.0f).dump("plain", plain).dump("warmer", warmer);

    auto spare = base;
    spare.dither = false;
    spare.grade = {};
    CHECK(tg::abs(co_await captured_middle(ctx, spare, "16", nothing) - plain) <= 0.0f);
}

ASYNC_INVOCABLE_TEST("sv - dither moves a value by under a step, the same way in every capture",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    auto const nothing = [](sv::view_ref const&, sr::display_transform&) {};
    auto const base = sr::display_transform{.exposure = -5.0f,
                                            .curve = sr::display_curve::neutral,
                                            .bloom = {.strength = 0.5f, .knee = 0.0f}};
    auto const plain = co_await captured_middle(ctx, base, "16", nothing);
    auto noisy = base;
    noisy.dither = true;
    auto const a = co_await captured_middle(ctx, noisy, "16", nothing);
    auto const b = co_await captured_middle(ctx, noisy, "16", nothing);
    CHECK(tg::abs(a - plain) <= 1.0f).dump("plain", plain).dump("dithered", a);
    CHECK(a == b);
}
