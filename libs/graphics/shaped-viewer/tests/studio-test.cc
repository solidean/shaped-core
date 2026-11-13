#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs, tg::exp

// Studio mode: the spec's dome turned to the camera, its projection, the framing, and the calibration a grey card reads.

namespace
{
using namespace cc::primitive_defines;

/// The dome's SH projection by brute force: equal-angle cells, each weighed by its solid angle.
void project_by_quadrature(sv::dome const& d, tg::vec3f (&sh)[sv::background::sh_coefficient_count])
{
    constexpr int rows = 128;
    constexpr int columns = 256;
    for (auto r = 0; r < rows; ++r)
    {
        auto const theta = tg::angle_f::make_from_radians((f32(r) + 0.5f) / f32(rows) * tg::pi<f32>);
        auto const weight = theta.sin() * (tg::pi<f32> / f32(rows)) * (2.0f * tg::pi<f32> / f32(columns));
        for (auto c = 0; c < columns; ++c)
        {
            auto const phi = tg::angle_f::make_from_radians((f32(c) + 0.5f) / f32(columns) * 2.0f * tg::pi<f32>);
            auto const x = theta.sin() * phi.cos();
            auto const y = theta.cos();
            auto const z = theta.sin() * phi.sin();
            f32 const basis[sv::background::sh_coefficient_count] = {
                0.282095f,
                0.488603f * y,
                0.488603f * z,
                0.488603f * x,
                1.092548f * x * y,
                1.092548f * y * z,
                0.315392f * (3.0f * z * z - 1.0f),
                1.092548f * x * z,
                0.546274f * (x * x - y * y),
                0.590044f * y * (3.0f * x * x - y * y),
                2.890611f * x * y * z,
                0.457046f * y * (5.0f * z * z - 1.0f),
                0.373176f * z * (5.0f * z * z - 3.0f),
                0.457046f * x * (5.0f * z * z - 1.0f),
                1.445306f * z * (x * x - y * y),
                0.590044f * x * (x * x - 3.0f * y * y),
            };
            auto const radiance = d.radiance(tg::vec3f(x, y, z));
            for (auto i = 0; i < sv::background::sh_coefficient_count; ++i)
                sh[i] += radiance * (basis[i] * weight);
        }
    }
}
} // namespace

TEST("sv::studio - the lobes turn with the camera, the key lighting a front view from its upper left")
{
    auto const d = sv::studio_dome();
    auto const front = sv::camera::orbiting(tg::pos3d(0, 0, 0), 5.0, tg::angle_d::make_from_degree(0.0), tg::angle_d());
    auto const seen = d.seen_from(front);
    CHECK(!seen.follows_camera);

    // The camera looks along +z from -z, its right +x: the key comes from the left (-x), above, and the camera's side (-z).
    auto const key = seen.lobes[0].direction;
    CHECK(key[0] < 0.0f);
    CHECK(tg::abs(key[1] - tg::angle_f::make_from_degree(45.0f).sin()) < 1e-5f);
    CHECK(key[2] < 0.0f);
    // The fill from the right, the rim from behind the subject.
    CHECK(seen.lobes[1].direction[0] > 0.0f);
    CHECK(seen.lobes[2].direction[2] > 0.0f);

    // From behind, the key is on the other side of the world.
    auto const back = sv::camera::orbiting(tg::pos3d(0, 0, 0), 5.0, tg::angle_d::make_from_degree(180.0), tg::angle_d());
    CHECK(d.seen_from(back).lobes[0].direction[2] > 0.0f);

    // Straight down there is no horizontal backward, and the spec takes -z.
    auto const top = sv::studio_camera(tg::aabb3f(tg::pos3f(-1, 0, -1), tg::pos3f(1, 2, 1)), sv::studio_view::top, 1.5);
    auto const from_top = d.seen_from(top).lobes[2].direction;
    CHECK(from_top[2] > 0.0f);
    CHECK(tg::abs(from_top[0]) < 1e-5f);

    // A frozen dome stays where it was turned.
    CHECK(seen.seen_from(back).lobes[0].direction[2] < 0.0f);
}

TEST("sv::studio - a dome projects to SH in closed form as it does by quadrature")
{
    // Turned, so the lobes lie off every axis and every band carries something.
    auto const d = sv::studio_dome().seen_from(sv::camera::orbiting(
        tg::pos3d(0, 0, 0), 5.0, tg::angle_d::make_from_degree(30.0), tg::angle_d::make_from_degree(20.0)));
    auto const closed = sv::background::of_dome(d);
    tg::vec3f reference[sv::background::sh_coefficient_count] = {};
    project_by_quadrature(d, reference);
    for (auto i = 0; i < sv::background::sh_coefficient_count; ++i)
        for (auto ch = 0; ch < 3; ++ch)
            CHECK(tg::abs(closed.sh[i][ch] - reference[i][ch]) < 0.01f * tg::abs(reference[0][ch]))
                .dump("coefficient", i)
                .dump("closed", closed.sh[i][ch])
                .dump("quadrature", reference[i][ch]);
}

TEST("sv::studio - the derived exposure shows a 0.19 card square to the key at sRGB 122")
{
    for (auto const setup : {sv::studio_setup::floor, sv::studio_setup::ceiling})
    {
        auto const d = sv::studio_dome(setup);
        auto const seen = d.seen_from(sv::camera{});
        auto const e = seen.irradiance(tg::normalize(seen.lobes[0].direction));
        auto const shown = sv::studio_card_albedo / tg::pi<f32> * e[1] * tg::exp(sv::studio_exposure(d) * 0.6931472f);
        // Below the neutral curve's knee, so the display shows it as it is: sRGB 122 is linear 0.1946.
        CHECK(tg::abs(shown - 0.19461f) < 1e-4f).dump("shown", shown);
    }
    // H scales the exposure by exactly its stops.
    CHECK(tg::abs(sv::studio_exposure(sv::studio_dome(sv::studio_setup::floor, 2.0f))
                  - (sv::studio_exposure(sv::studio_dome()) - 1.0f))
          < 1e-3f);
}

// A hit on a triangle is only as precise as its corners' coordinates: one pair a kilometer across put every hit's height off by
// more than a continuation's offset, and the floor struck itself in square rings around the origin.
// So the floor reaches as far as asked, and the cells around the asset are small.
TEST("sv::studio - the floor reaches its extent in a grid whose cells shrink toward the origin")
{
    auto const floor = sv::studio_floor(0.25f, 1000.0f);
    auto const& g = floor.geometry;
    REQUIRE(g.is_indexed());

    auto reach = 0.0f;
    for (auto const p : g.positions)
    {
        CHECK(p[1] == 0.25f);
        reach = cc::max(reach, cc::max(tg::abs(p[0]), tg::abs(p[2])));
    }
    CHECK(reach == 1000.0f);

    // the smallest cell at the origin, the corner whose other coordinates are the least positive line
    auto nearest = 1000.0f;
    for (auto const p : g.positions)
        if (p[0] > 0.0f)
            nearest = cc::min(nearest, p[0]);
    CHECK(nearest < 0.01f);

    // every triangle faces up, so the floor is lit and casts its shadows from above
    for (auto t = isize(0); t < g.triangle_count(); ++t)
    {
        auto const a = g.positions[g.indices[3 * t]];
        auto const b = g.positions[g.indices[3 * t + 1]];
        auto const c = g.positions[g.indices[3 * t + 2]];
        CHECK(tg::cross(b - a, c - a)[1] > 0.0f).context(cc::format("triangle {}", t));
    }
}

TEST("sv::studio - the camera frames the bounds at 50 mm on a 36 mm-wide sensor")
{
    // Long in x like a car, so the horizontal edge is the one the box reaches.
    auto const bounds = tg::aabb3f(tg::pos3f(-3, 0, -1), tg::pos3f(3, 1.5f, 1));
    auto const aspect = 16.0 / 9.0;
    auto const cam = sv::studio_camera(bounds, sv::studio_view::front, aspect);
    // 36 mm wide at 16:9 is 20.25 mm tall.
    auto const tan_v = 18.0 / 50.0 / aspect;
    CHECK(tg::abs(cam.projection.vertical_fov.radians() - 2.0 * tg::atan(tan_v).radians()) < 1e-9);

    // Every corner inside the frame, and one at its edge with the tenth to spare.
    auto const basis = cam.basis();
    auto widest = 0.0;
    for (auto corner = 0; corner < 8; ++corner)
    {
        auto const p = tg::pos3d(corner & 1 ? 3.0 : -3.0, corner & 2 ? 1.5 : 0.0, corner & 4 ? 1.0 : -1.0);
        auto const rel = p - cam.position;
        auto const z = tg::dot(rel, basis.forward);
        auto const sx = tg::abs(tg::dot(rel, basis.right)) / (z * 18.0 / 50.0);
        auto const sy = tg::abs(tg::dot(rel, basis.up)) / (z * tan_v);
        CHECK(sx <= 1.0 / 1.1 + 1e-9);
        CHECK(sy <= 1.0 / 1.1 + 1e-9);
        widest = cc::max(widest, cc::max(sx, sy));
    }
    CHECK(tg::abs(widest - 1.0 / 1.1) < 1e-9);
    // In front, looking along +z; and turned, the front view looks along the front.
    CHECK(basis.forward[2] > 0.9);
    auto const turned = sv::studio_camera(bounds, sv::studio_view::front, aspect, tg::angle_d::make_from_degree(-90.0));
    CHECK(turned.basis().forward[0] > 0.9);
}

namespace
{
/// The mean display value of a diffuse 0.19 card square to the studio's key, filling the image's middle, traced under the studio dome and its derived exposure.
cc::shared_async<f32> captured_card(sg::context& ctx)
{
    constexpr auto size = tg::vec2i(64, 48);
    auto const path = cc::format("{}/sv-studio-card.png", cc::temp_directory_path());
    auto const on = cc::scoped_environment_variable(sr::capture_request_env_var, "1");
    auto const out = cc::scoped_environment_variable(sr::capture_output_env_var, path);
    auto const which = cc::scoped_environment_variable(sr::capture_name_env_var, "front");
    auto const dim = cc::scoped_environment_variable(sr::capture_size_env_var, cc::format("{}x{}", size[0], size[1]));
    auto const acc = cc::scoped_environment_variable(sr::capture_accumulate_env_var, "64");
    auto const lim = cc::scoped_environment_variable(sr::capture_timeout_env_var, "60");

    auto& lib = sv_test::shared_material_library();
    auto const card_material = lib.acquire(sv::material::create(
        "grey card", lib.acquire_type(sv::builtin_material::openpbr).value(),
        {sv::material_attribute_binding::of(
             "base_color", tg::vec3f(sv::studio_card_albedo, sv::studio_card_albedo, sv::studio_card_albedo)),
         sv::material_attribute_binding::of("specular_weight", 0.0f)}));

    // The card faces the key as this camera turns it, centered on the origin and larger than the view.
    auto const camera = sv::camera::orbiting(tg::pos3d(0, 0, 0), 3.0, tg::angle_d(), tg::angle_d());
    auto const d = sv::studio_dome();
    auto const n = tg::normalize(d.seen_from(camera).lobes[0].direction);
    auto const u = tg::normalize(tg::dual(tg::cross(tg::vec3f(0, 1, 0), n)));
    auto const v = tg::dual(tg::cross(n, u));
    auto const corner = [&](f32 a, f32 b) { return tg::pos3f::zero + u * (4.0f * a) + v * (4.0f * b); };
    auto const card
        = sv::mesh{.name = "grey card",
                   .geometry = sv::triangle_geometry::create_from_positions(cc::vector<tg::pos3f>{
                       corner(-1, -1), corner(1, 1), corner(1, -1), corner(-1, -1), corner(-1, 1), corner(1, 1)}),
                   .material = card_material};

    auto const start = cc::current_time_steady_secs();
    for (auto f : sv::interactive(ctx, "sv-test/studio-card"))
    {
        auto view = f.window().view();
        view.camera(camera);
        f.register_capture("front", [](sv::capture_context const&) {});
        auto scene = view.add_scene();
        scene.add_mesh(card);
        scene.settings({.mode = sv::render_mode::megakernel});
        scene.studio({.draw_floor = false});
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
    for (auto y = 20; y < 28; ++y)
        for (auto x = 28; x < 36; ++x)
            for (auto ch = 0; ch < 3; ++ch)
            {
                sum += f32(img.pixels[(y * img.width + x) * img.channels + ch]);
                ++count;
            }
    cc::remove_file(path);
    co_return sum / f32(count);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv::studio - a 0.19 grey card square to the key reads sRGB 122", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx))
        SKIP("no ray tracing or no SGL compiler, so the tracer does not build");

    // The exposure is derived from the dome alone, so no floor is drawn and nothing but the dome surrounds the card.
    auto const value = co_await captured_card(ctx);
    CHECK(tg::abs(value - 122.0f) <= 2.0f).dump("card", value);
}
