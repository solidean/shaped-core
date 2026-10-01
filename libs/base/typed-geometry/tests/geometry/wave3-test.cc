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
