#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <clean-core/streams/file_stream.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// Quadrics on the raster path: each primitive drawn as its box, the surface solved per pixel, and its depth written.
// The megakernel traces the same set, so it is the reference the raster, deferred and forward images are held to.

namespace
{
using namespace cc::primitive_defines;

constexpr auto k_size = tg::vec2i(64, 64);

/// A red sphere of `radius` at the origin, as one quadric batch.
[[nodiscard]] sv::quadric_set sphere_set(f32 radius)
{
    auto set = sv::quadric_set();
    set.name = cc::format("raster sphere {}", radius);
    set.add_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), radius));
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(0.8f, 0.1f, 0.1f)};
    set.attributes.push_back(sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, colors));
    return set;
}

/// `set` seen through `cam`, drawn the `mode` and `lighting` way, captured once it holds 32 samples.
cc::shared_async<babel::image::image> capture(sg::context& ctx,
                                              sv::quadric_set const& set,
                                              sv::camera cam,
                                              sv::render_mode mode,
                                              sv::hybrid_lighting lighting)
{
    auto const path = cc::temp_file_path("sv-quadric-raster", ".png");
    {
        auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
        auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
        auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
        auto const dim
            = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", k_size[0], k_size[1]));
        auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "32");
        auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "120");

        cam.projection.aspect_ratio = 1.0;
        auto const loop_start = cc::current_time_steady_secs();
        for (auto f : sv::interactive(ctx, "sv-test/quadric-raster"))
        {
            f.register_capture("front", [](sv::capture_context const&) {});
            auto view = f.window().view();
            view.camera(cam);
            auto scene = view.add_scene();
            (void)scene.add_quadrics(set);
            // TEMPORARY(bug-sv-quadric-only-capture-blank): a view holding only quadrics captures blank, so one triangle stands behind the camera.
            tg::pos3f const behind[] = {tg::pos3f(0, 0, -20), tg::pos3f(1, 0, -20), tg::pos3f(0, 1, -20)};
            sv_test::pbr_material const gray[] = {{}};
            (void)scene.add_mesh(sv_test::as_mesh("behind", behind, gray));
            scene.background(sv::background::uniform(tg::vec3f(0.5f, 0.5f, 0.5f)));
            scene.settings(
                {.mode = mode, .hybrid_lighting = lighting, .reconstruct = {.denoiser = sr::denoise_method::none}});
            REQUIRE(cc::current_time_steady_secs() - loop_start < 110.0);
        }
        co_await cc::async_settled(sv::background_work(ctx));
    }

    auto reread = cc::file_read_stream_adapter::open(path);
    REQUIRE(reread.has_value());
    cc::read_stream in = reread.value();
    auto decoded = babel::image::read(in);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().width == k_size[0]);
    REQUIRE(decoded.value().height == k_size[1]);
    cc::remove_file(path);
    co_return cc::move(decoded.value());
}

/// Channel `c` of pixel `i`, in [0, 255].
[[nodiscard]] int channel(babel::image::image const& img, isize i, int c)
{
    return int(img.pixels[i * img.channels + c]);
}

/// Whether pixel `i` shows the gray sky, 0.5 in every channel, which nothing in these scenes is.
[[nodiscard]] bool is_sky(babel::image::image const& img, isize i)
{
    auto const near_sky = [&](int c) { return tg::abs(channel(img, i, c) - 127) < 12; };
    return near_sky(0) && near_sky(1) && near_sky(2);
}

/// Whether pixel `i` shows the red sphere rather than the gray sky.
[[nodiscard]] bool is_sphere(babel::image::image const& img, isize i)
{
    return channel(img, i, 0) > channel(img, i, 1) + 24;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a quadric draws on the raster path where the tracer draws it, and from inside its box",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    struct contender
    {
        char const* name = "";
        sv::render_mode mode = sv::render_mode::hybrid;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred;
        /// whether its color is held to the tracer's, which a raster frame's unshadowed lighting is not
        bool lit_as_traced = true;
    };
    auto const contenders = cc::array<contender>{
        contender{.name = "raster", .mode = sv::render_mode::raster, .lit_as_traced = false},
        contender{.name = "hybrid deferred", .lighting = sv::hybrid_lighting::deferred},
        contender{.name = "hybrid forward", .lighting = sv::hybrid_lighting::forward},
    };
    auto const pixels = isize(k_size[0]) * isize(k_size[1]);

    // Outside: the sphere covers the pixels the tracer's does, lit as the tracer lights it.
    {
        auto const set = sphere_set(1.0f);
        auto const cam = sv::camera::looking_at(tg::pos3d(0.3, 0.2, -4), tg::pos3d(0, 0, 0));
        auto const reference
            = co_await capture(ctx, set, cam, sv::render_mode::megakernel, sv::hybrid_lighting::deferred);
        auto covered = 0;
        auto red = 0.0;
        for (auto i = isize(0); i < pixels; ++i)
            if (is_sphere(reference, i))
            {
                ++covered;
                red += f64(channel(reference, i, 0));
            }
        REQUIRE(covered > pixels / 10);
        red /= f64(covered);

        for (auto const& c : contenders)
        {
            auto const seen = co_await capture(ctx, set, cam, c.mode, c.lighting);
            auto differing = 0;
            auto seen_covered = 0;
            auto seen_red = 0.0;
            for (auto i = isize(0); i < pixels; ++i)
            {
                if (is_sphere(seen, i) != is_sphere(reference, i))
                    ++differing;
                if (is_sphere(seen, i))
                {
                    ++seen_covered;
                    seen_red += f64(channel(seen, i, 0));
                }
            }
            seen_red /= f64(cc::max(seen_covered, 1));
            CC_LOG_INFO("{}: {} of {} sphere pixels differ from the tracer's; red {} against the tracer's {}", c.name,
                        differing, covered, seen_red, red);
            // The silhouette's own pixels may round either way.
            CHECK(differing < covered / 20).context(c.name).dump("differing", differing).dump("covered", covered);
            if (c.lit_as_traced)
                CHECK(tg::abs(seen_red - red) < 0.1 * red).context(c.name).dump("red", seen_red).dump("traced", red);
        }
    }

    // Inside: the camera stands within the large sphere's box, so only the box's far faces are drawn, and they cover every pixel.
    // A traced frame lights the inside of a closed sphere by nothing, so it is black there; what counts is that no pixel shows the sky.
    {
        auto const set = sphere_set(5.0f);
        auto const cam = sv::camera::looking_at(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 1));
        for (auto const& c : contenders)
        {
            auto const seen = co_await capture(ctx, set, cam, c.mode, c.lighting);
            auto covered = 0;
            for (auto i = isize(0); i < pixels; ++i)
                if (!is_sky(seen, i))
                    ++covered;
            CC_LOG_INFO("{} from inside: {} of {} pixels show the sphere", c.name, covered, pixels);
            CHECK(covered == pixels).context(c.name).dump("covered", covered);
        }
    }
}
