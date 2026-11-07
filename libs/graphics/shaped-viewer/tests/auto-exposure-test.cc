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

// A layer at an automatic exposure: metered from its own image, and adapted until that image's mean reads middle gray.
//
// A diffuse quad filling the view under a uniform sky is the one scene whose answer is known: its radiance is its albedo times the sky.
// So at any sky brightness, the exposure has to land on 0.18 over that, which the neutral curve shows as sRGB 118.

namespace
{
using namespace cc::primitive_defines;

/// The mean display value in the middle of a quad of albedo 0.5 filling the view, under a sky of `sky` nits, at an automatic exposure.
cc::shared_async<f32> captured_middle(sg::context& ctx, f32 sky, u32 samples)
{
    constexpr auto size = tg::vec2i(48, 32);
    auto const path = cc::format("{}/sv-auto-exposure.png", cc::temp_directory_path());
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, cc::to_string(samples));
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "60");

    auto& lib = sv_test::shared_material_library();
    auto const gray = lib.acquire(
        sv::material::create("auto exposure gray", lib.acquire_type(sv::builtin_material::openpbr).value(),
                             {sv::material_attribute_binding::of("base_color", tg::vec3f(0.5f, 0.5f, 0.5f)),
                              sv::material_attribute_binding::of("specular_weight", 0.0f)}));
    // Facing the camera at -z, far wider than the view.
    auto const e = 20.0f;
    auto const quad = sv::mesh{.name = "auto exposure quad",
                               .geometry = sv::triangle_geometry::create_from_positions(cc::vector<tg::pos3f>{
                                   tg::pos3f(-e, -e, 0), tg::pos3f(e, e, 0), tg::pos3f(e, -e, 0), tg::pos3f(-e, -e, 0),
                                   tg::pos3f(-e, e, 0), tg::pos3f(e, e, 0)}),
                               .material = gray};

    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/auto-exposure"))
    {
        auto view = f.window().view();
        view.camera(sv::camera::looking_at(tg::pos3d(0, 0, -3), tg::pos3d(0, 0, 0)));
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(quad);
        scene.background(sv::background::uniform(tg::vec3f(sky, sky, sky)));
        scene.fallback_light(cc::nullopt);
        scene.settings(
            {.mode = sv::render_mode::megakernel, .display = {.curve = sr::display_curve::neutral, .automatic = true}});
        REQUIRE(cc::current_time_steady_secs() - start < 55.0);
    }
    co_await cc::async_settled(sv::background_work(ctx));

    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto const decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());
    auto const& img = decoded.value();
    REQUIRE(img.width == size[0]);

    auto sum = 0.0f;
    auto count = 0;
    for (auto y = 12; y < 20; ++y)
        for (auto x = 20; x < 28; ++x)
            for (auto ch = 0; ch < 3; ++ch)
            {
                sum += f32(img.pixels[(y * img.width + x) * img.channels + ch]);
                ++count;
            }
    cc::remove_file(path);
    co_return sum / f32(count);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - an automatic exposure shows an image's mean as middle gray, whatever its brightness",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    // Fifty times apart, and both read sRGB 118, the neutral curve's 0.18.
    for (auto const sky : {1.0f, 50.0f})
    {
        auto const value = co_await captured_middle(ctx, sky, 512);
        CHECK(tg::abs(value - 118.0f) <= 3.0f).dump("sky", sky).dump("middle", value);
    }
}

ASYNC_INVOCABLE_TEST("sv - a still capture shows its automatic exposure as read off the finished image, however few "
                     "frames it took",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    // Four samples a frame, so this finishes in two frames, long before an exposure adapting by a fifth per reading could.
    auto const value = co_await captured_middle(ctx, 1.0f, 8);
    CHECK(tg::abs(value - 118.0f) <= 3.0f).dump("middle", value);
}
