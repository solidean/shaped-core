#include <nexus/test.hh>
#include <shaped-xr/impl/view_math.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// OpenXR's frame is right-handed with -z forward, tg's left-handed with +z forward, and the session converts every pose
// and field of view at its edge.
// What a headset shows is only right where both conversions are.

namespace
{
[[nodiscard]] bool near(f32 a, f32 b)
{
    return tg::abs(a - b) < 1e-5f;
}

/// `m * (p, 1)`, divided by w.
[[nodiscard]] tg::vec3f projected(tg::mat4f const& m, tg::vec3f p)
{
    auto const c = m * tg::vec4f(p[0], p[1], p[2], 1.0f);
    return tg::vec3f(c[0] / c[3], c[1] / c[3], c[2] / c[3]);
}
} // namespace

TEST("shaped-xr view - an OpenXR pose is the same place and direction in tg's frame, its z mirrored")
{
    // OpenXR's forward is -z and tg's is +z, so a position ahead stays ahead
    auto const placed = sxr::impl::camera_to_world_of({.px = 1, .py = 2, .pz = -3});
    auto const at = placed.to_mat() * tg::vec4f(0, 0, 0, 1);
    CHECK(near(at[0], 1));
    CHECK(near(at[1], 2));
    CHECK(near(at[2], 3));

    // a quarter turn to the left about +y in OpenXR: its forward, -z, becomes -x, and tg's forward, +z, must too
    auto const s = tg::sin(tg::angle_f::make_from_degree(45.0f));
    auto const turned = sxr::impl::camera_to_world_of({.qy = s, .qw = s});
    auto const forward = turned.to_mat() * tg::vec4f(0, 0, 1, 0);
    CHECK(near(forward[0], -1));
    CHECK(near(forward[1], 0));
    CHECK(near(forward[2], 0));
    // and up stays up
    auto const up = turned.to_mat() * tg::vec4f(0, 1, 0, 0);
    CHECK(near(up[1], 1));
}

TEST("shaped-xr view - an eye's projection maps its four angles to the clip edges, and near and far to 0 and 1")
{
    // an asymmetric frustum, as every headset eye is: wider towards the nose on one side
    auto const fov = sxr::impl::openxr_fov{.left = -0.8f, .right = 0.6f, .up = 0.7f, .down = -0.9f};
    auto const m = sxr::impl::projection_of(fov, 0.1f, 50.0f).to_mat();
    auto const d = 2.0f;
    auto const left = projected(m, tg::vec3f(tg::tan(tg::angle_f::make_from_radians(fov.left)) * d, 0, d));
    auto const right = projected(m, tg::vec3f(tg::tan(tg::angle_f::make_from_radians(fov.right)) * d, 0, d));
    auto const up = projected(m, tg::vec3f(0, tg::tan(tg::angle_f::make_from_radians(fov.up)) * d, d));
    auto const down = projected(m, tg::vec3f(0, tg::tan(tg::angle_f::make_from_radians(fov.down)) * d, d));
    CHECK(near(left[0], -1));
    CHECK(near(right[0], 1));
    CHECK(near(up[1], 1));
    CHECK(near(down[1], -1));
    CHECK(near(projected(m, tg::vec3f(0, 0, 0.1f))[2], 0));
    CHECK(near(projected(m, tg::vec3f(0, 0, 50.0f))[2], 1));
}

TEST("shaped-xr view - world_to_clip puts a point straight ahead of the eye in the middle of its image")
{
    auto const view = sxr::view{.camera_to_world = sxr::impl::camera_to_world_of({.px = 0.5f, .py = 1.6f, .pz = 0.0f}),
                                .projection = sxr::impl::projection_of(
                                    {.left = -0.7f, .right = 0.7f, .up = 0.7f, .down = -0.7f}, 0.05f, 100.0f)};
    // one metre ahead in OpenXR, z = -1, is z = +1 in tg's frame
    auto const ahead = projected(view.world_to_clip(), tg::vec3f(0.5f, 1.6f, 1.0f));
    CHECK(near(ahead[0], 0));
    CHECK(near(ahead[1], 0));
    CHECK(ahead[2] > 0.0f);
    CHECK(ahead[2] < 1.0f);
}
