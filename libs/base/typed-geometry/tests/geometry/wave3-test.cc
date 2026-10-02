#include "../approx.hh"

#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// Wave 3: the infinite cylinder and cone, and the frustum.

namespace
{
static_assert(!tg::traits::is_finite<tg::inf_cone3d>);
static_assert(!tg::has_distance_sqr_to<tg::inf_cylinder3d, tg::aabb3d>, "unbounded: no support, so no GJK");
static_assert(tg::has_separation_from<tg::frustum3d, tg::sphere3d>, "a frustum has its corners as a support");

/// a box-shaped frustum, the cube [-1, 1]^3, so every answer is easy to check
tg::frustum3d unit_frustum()
{
    return tg::frustum3d(tg::plane3d(tg::vec3d(-1, 0, 0), 1), tg::plane3d(tg::vec3d(1, 0, 0), 1),
                         tg::plane3d(tg::vec3d(0, -1, 0), 1), tg::plane3d(tg::vec3d(0, 1, 0), 1),
                         tg::plane3d(tg::vec3d(0, 0, -1), 1), tg::plane3d(tg::vec3d(0, 0, 1), 1));
}

/// a perspective-like frustum: apex at the origin looking down +z, near at 1, far at 3, 45 degrees to each side
tg::frustum3d view_frustum()
{
    auto const s = 1 / tg::sqrt(2.0);
    return tg::frustum3d(tg::plane3d(tg::vec3d(-s, 0, -s), 0), tg::plane3d(tg::vec3d(s, 0, -s), 0),
                         tg::plane3d(tg::vec3d(0, -s, -s), 0), tg::plane3d(tg::vec3d(0, s, -s), 0),
                         tg::plane3d(tg::vec3d(0, 0, -1), -1), tg::plane3d(tg::vec3d(0, 0, 1), 3));
}

/// left-handed, looking down +z, 90 degrees vertically, square; reverse-Z with near 1, and far 10 or at infinity
tg::frustum3d reverse_z_frustum(bool infinite_far)
{
    auto const n = 1.0;
    auto const f = 10.0;
    auto m = tg::mat4d::zero;
    m[0, 0] = 1.0;
    m[1, 1] = 1.0;
    m[2, 2] = infinite_far ? 0.0 : -n / (f - n);
    m[3, 2] = infinite_far ? n : n * f / (f - n);
    m[2, 3] = 1.0;
    return tg::frustum3d::make_from_view_projection(tg::projective_transform3d::make_from_mat(m));
}

bool approx_planes(tg::frustum3d const& a, tg::frustum3d const& b)
{
    for (auto i = 0; i < 6; ++i)
        if (!tgtest::approx(a.planes[i].normal, b.planes[i].normal, 1e-12)
            || !tgtest::approx(a.planes[i].dist, b.planes[i].dist, 1e-12))
            return false;
    return true;
}
} // namespace

TEST("tg wave3 - infinite cylinder")
{
    auto const c = tg::inf_cylinder3d(tg::line3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1)), 1.0);
    CHECK(c.contains(tg::pos3d(0.5, 0.5, 100)));
    CHECK(tgtest::approx(tg::pos3d(3, 0, -50).distance_to(c), 2.0));
    CHECK(tgtest::approx(tg::pos3d(0.5, 0, 7).project_to(c.boundary()), tg::pos3d(1, 0, 7)));
    CHECK(c.intersects(tg::sphere3d(tg::pos3d(2.5, 0, 9), 1.6)));
    CHECK(!c.intersects(tg::sphere3d(tg::pos3d(2.5, 0, 9), 1.4)));

    auto const across = tg::line3d(tg::pos3d(-5, 0, 3), tg::vec3d(1, 0, 0));
    CHECK(across.intersection_parameter_with(c.boundary()).size() == 2);
    // a line along the axis inside the tube never crosses it, and lies inside it all the way
    auto const along = tg::line3d(tg::pos3d(0.5, 0, 0), tg::vec3d(0, 0, 1));
    CHECK(along.intersection_parameter_with(c.boundary()).is_empty());
    CHECK(along.intersects(c));

    auto const finite = tg::cylinder3d(tg::segment3d(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 2)), 1.0);
    CHECK(finite.unbounded() == tg::inf_cylinder3d(tg::line3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 2)), 1.0));
}

TEST("tg wave3 - infinite cone")
{
    // a 90 degree opening along +z: the surface is z == rho
    auto const c = tg::inf_cone3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1), tg::angle_d::make_from_degree(90));
    CHECK(c.contains(tg::pos3d(0.5, 0, 1)));
    CHECK(!c.contains(tg::pos3d(2, 0, 1)));
    CHECK(!c.contains(tg::pos3d(0, 0, -1)));
    CHECK(tgtest::approx(tg::pos3d(2, 0, 0).distance_to(c), tg::sqrt(2.0), 1e-12));
    // a point on the axis is a special case for its surface: any direction around the axis is as near
    CHECK(tgtest::approx(tg::pos3d(0, 0, 2).distance_to(c.boundary()), tg::sqrt(2.0), 1e-12));

    auto const r = tg::ray3d(tg::pos3d(-5, 0, 2), tg::vec3d(1, 0, 0));
    auto const in = r.intersection_parameter_with(c).value();
    CHECK(tgtest::approx(in.start, 3.0, 1e-12));
    CHECK(tgtest::approx(in.end, 7.0, 1e-12));
    // a ray up the axis from below enters at the apex and never leaves
    auto const up = tg::ray3d(tg::pos3d(0, 0, -2), tg::vec3d(0, 0, 1));
    CHECK(tgtest::approx(up.closest_intersection_parameter_with(c).value(), 2.0, 1e-12));
    CHECK(up.intersection_parameter_with(c.boundary()).size() == 1);

    auto const finite = tg::cone3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 2), 2.0);
    CHECK(tgtest::approx(finite.unbounded().opening_angle.degree(), 90.0, 1e-9));
}

TEST("tg wave3 - frustum")
{
    auto const box = unit_frustum();
    auto const v = box.vertices();
    CHECK(tgtest::approx(v[0], tg::pos3d(-1, -1, -1)));
    CHECK(tgtest::approx(v[7], tg::pos3d(1, 1, 1)));
    CHECK(tgtest::approx(box.volume(), 8.0, 1e-12));
    CHECK(tgtest::approx(box.centroid(), tg::pos3d(0, 0, 0)));

    auto const f = view_frustum();
    // near square half-size 1, far 3: a truncated pyramid of volume h/3 (A1 + A2 + sqrt(A1 A2)) = 2/3 (4 + 36 + 12)
    CHECK(tgtest::approx(f.volume(), 2.0 / 3.0 * 52.0, 1e-9));
    CHECK(f.contains(tg::pos3d(0, 0, 2)));
    CHECK(!f.contains(tg::pos3d(0, 0, 0.5)));
    CHECK(!f.contains(tg::pos3d(2.5, 0, 2)));

    CHECK(f.intersects(tg::sphere3d(tg::pos3d(0, 0, 5), 2.1)));
    CHECK(!f.intersects(tg::sphere3d(tg::pos3d(0, 0, 5), 1.9)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 5).distance_to(f), 2.0, 1e-6));

    auto const look = tg::ray3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1));
    auto const in = look.intersection_parameter_with(f).value();
    CHECK(tgtest::approx(in.start, 1.0));
    CHECK(tgtest::approx(in.end, 3.0));
    CHECK(look.intersection_parameter_with(f.boundary()).size() == 2);

    auto rng = cc::random(53);
    for (auto i = 0; i < 300; ++i)
        CHECK(f.contains(f.sample_uniform(rng)));
}

TEST("tg wave3 - a frustum from a reverse-Z view-projection, and culling")
{
    // left-handed, looking down +z, 90 degrees vertically, square; reverse-Z: near 1 -> depth 1, far 10 -> depth 0
    auto const n = 1.0;
    auto const f = 10.0;
    auto m = tg::mat4d::zero;
    m[0, 0] = 1.0;
    m[1, 1] = 1.0;
    m[2, 2] = -n / (f - n);
    m[3, 2] = n * f / (f - n);
    m[2, 3] = 1.0;
    auto const vp = tg::projective_transform3d::make_from_mat(m);

    auto const fr = tg::frustum3d::make_from_view_projection(vp);
    CHECK(fr.contains(tg::pos3d(0, 0, 5)));
    CHECK(!fr.contains(tg::pos3d(0, 0, 0.5)));
    CHECK(!fr.contains(tg::pos3d(0, 0, 11)));
    CHECK(!fr.contains(tg::pos3d(6, 0, 5)));
    CHECK(tgtest::approx(fr.vertices()[0], tg::pos3d(-1, -1, 1), 1e-9));
    CHECK(tgtest::approx(fr.vertices()[7], tg::pos3d(10, 10, 10), 1e-9));

    // culling agrees with the exact test away from the corners, and may only err towards true
    CHECK(fr.may_intersect(tg::sphere3d(tg::pos3d(0, 0, 5), 1.0)));
    CHECK(!fr.may_intersect(tg::sphere3d(tg::pos3d(0, 0, -5), 1.0)));
    CHECK(!fr.may_intersect(tg::aabb3d(tg::pos3d(20, -1, 4), tg::pos3d(22, 1, 6))));
    auto rng = cc::random(59);
    for (auto i = 0; i < 300; ++i)
    {
        auto const s
            = tg::sphere3d(tg::pos3d(rng.uniform(-15.0, 15.0), rng.uniform(-15.0, 15.0), rng.uniform(-5.0, 15.0)), 1.0);
        if (fr.intersects(s))
            CHECK(fr.may_intersect(s));
        CHECK(s.may_intersect(fr) == fr.may_intersect(s));
    }

    // every other pair falls back to the exact test
    CHECK(tg::sphere3d(tg::pos3d(0, 0, 0), 1.0).may_intersect(tg::pos3d(0.5, 0, 0)));
}

TEST("tg wave3 - an infinite reverse-Z projection gives a frustum without a far plane")
{
    // reverse-Z with the far plane at infinity: depth = near / z, so z_clip is the constant near
    auto const n = 1.0;
    auto m = tg::mat4d::zero;
    m[0, 0] = 1.0;
    m[1, 1] = 1.0;
    m[3, 2] = n;
    m[2, 3] = 1.0;
    auto const fr = tg::frustum3d::make_from_view_projection(tg::projective_transform3d::make_from_mat(m));

    CHECK(!fr.has_far_plane());
    CHECK(tg::frustum3d::make_from_view_projection(tg::projective_transform3d::make_from_mat(tg::mat4d::identity))
              .has_far_plane());

    CHECK(fr.contains(tg::pos3d(0, 0, 1e9)));
    CHECK(!fr.contains(tg::pos3d(0, 0, 0.5)));
    CHECK(!fr.contains(tg::pos3d(1e9 + 1, 0, 1e9)));

    CHECK(fr.may_intersect(tg::sphere3d(tg::pos3d(0, 0, 1e6), 1.0)));
    CHECK(!fr.may_intersect(tg::sphere3d(tg::pos3d(0, 0, -5), 1.0)));

    // the view ray enters at the near plane and never leaves
    auto const in = tg::ray3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1)).intersection_parameter_with(fr).value();
    CHECK(tgtest::approx(in.start, 1.0));
    CHECK(in.end > 1e30);
}

TEST("tg wave3 - the boundary of a frustum without a far plane has no crossing at infinity")
{
    auto const fr = reverse_z_frustum(true);

    // from the camera: it crosses the near plane, and the open end is not a second crossing
    auto const from_camera = tg::ray3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1)).intersection_parameter_with(fr.boundary());
    REQUIRE(from_camera.size() == 1);
    CHECK(tgtest::approx(from_camera.first(), 1.0));

    // from inside, looking down the view axis: it never leaves
    auto const inside = tg::ray3d(tg::pos3d(0, 0, 2), tg::vec3d(0, 0, 1));
    CHECK(inside.intersection_parameter_with(fr.boundary()).is_empty());
    CHECK(!inside.intersects(fr.boundary()));

    // with a far plane the same ray still leaves through it
    CHECK(inside.intersection_parameter_with(reverse_z_frustum(false).boundary()).size() == 1);
}

TEST("tg wave3 - a transformed frustum keeps its inside, mirrored or without a far plane")
{
    auto const fr = reverse_z_frustum(false);
    auto const mirror = tg::signed_scaling_transform3d::make_scaling(tg::vec3d(-1, 1, 1));
    auto const mirrored = fr.transformed(mirror);
    CHECK(mirrored.contains(tg::pos3d(0, 0, 5)));
    CHECK(mirrored.contains(tg::pos3d(-2, 1, 5)));
    CHECK(!mirrored.contains(tg::pos3d(0, 0, 11)));
    CHECK(approx_planes(mirrored.transformed(mirror), fr));

    // the absent far plane's zero normal maps to zero, so it stays absent under a translation and a mirror alike
    auto const open = reverse_z_frustum(true);
    auto const moved = open.transformed(tg::translation_transform3d::make_translation(tg::vec3d(1, 0, 0)));
    CHECK(!moved.has_far_plane());
    CHECK(moved.contains(tg::pos3d(1, 0, 1e9)));
    auto const open_mirrored = open.transformed(mirror);
    CHECK(!open_mirrored.has_far_plane());
    CHECK(open_mirrored.contains(tg::pos3d(0, 0, 1e9)));
    CHECK(!open_mirrored.contains(tg::pos3d(0, 0, 0.5)));
}

TEST("tg wave3 - a negative uniform scale turns an infinite cone around")
{
    auto const c = tg::inf_cone3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 1), tg::angle_d::make_from_degree(60));
    auto const r = c.transformed(tg::signed_similarity_transform3d::make_uniform_scaling(-1.0));
    CHECK(tgtest::approx(r.dir, tg::vec3d(0, 0, -1)));
    CHECK(r.contains(tg::pos3d(0, 0, -1)));
    CHECK(!r.contains(tg::pos3d(0, 0, 1)));
    auto const doubled = c.transformed(tg::signed_similarity_transform3d::make_uniform_scaling(-2.0));
    CHECK(tgtest::approx(doubled.dir, tg::vec3d(0, 0, -1)));
}

TEST("tg wave3 - a point meets a frustum's surface exactly when it lies on a face")
{
    static_assert(tg::has_intersects<tg::pos3d, tg::frustum3d_surface>);
    static_assert(tg::has_intersects<tg::frustum3d_surface, tg::pos3d>);

    auto const s = unit_frustum().boundary();
    CHECK(s.contains(tg::pos3d(1, 0, 0)));
    CHECK(s.intersects(tg::pos3d(1, 1, 1)));
    CHECK(tg::pos3d(0, -1, 0.5).intersects(s));
    CHECK(!s.intersects(tg::pos3d(0, 0, 0)));
    CHECK(!tg::pos3d(0, 0, 0).intersects(s));
    CHECK(!s.intersects(tg::pos3d(2, 0, 0)));

    // an absent far plane is satisfied with equality everywhere, and still no point lies on it
    auto const open = reverse_z_frustum(true);
    CHECK(open.contains(tg::pos3d(0, 0, 5)));
    CHECK(!open.boundary().intersects(tg::pos3d(0, 0, 5)));
}
