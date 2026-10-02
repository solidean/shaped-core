#include "../approx.hh"

#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The round objects swept along a segment: the capsule, the cylinder, its surface and its tube.

namespace
{
static_assert(tg::traits::intrinsic_dim<tg::capsule3f> == 3);
static_assert(tg::traits::intrinsic_dim<tg::tube3f> == 2);
static_assert(std::is_same_v<tg::tube3f, tg::cylinder3f_mantle>);
static_assert(tg::impl::is_boundary<tg::cylinder3f_surface>);
static_assert(!tg::impl::is_boundary<tg::tube3f>, "a tube is only part of its solid's boundary");
static_assert(tg::has_distance_sqr_to<tg::capsule3d, tg::capsule3d>, "a capsule has a support, so GJK serves it");
static_assert(tg::has_separation_from<tg::cylinder3d, tg::aabb3d>);

auto const axis = tg::segment3d(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 4));
} // namespace

TEST("tg round - capsule")
{
    auto const c = tg::capsule3d(axis, 1.0);
    auto const pi = tg::pi<double>;

    CHECK(tgtest::approx(c.volume(), pi * 4 + 4.0 / 3.0 * pi, 1e-9));
    CHECK(tgtest::approx(c.area(), 8 * pi + 4 * pi, 1e-9));
    CHECK(c.bounds() == tg::aabb3d(tg::pos3d(-1, -1, -1), tg::pos3d(1, 1, 5)));

    CHECK(tgtest::approx(tg::pos3d(3, 0, 2).distance_to(c), 2.0));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 7).distance_to(c), 2.0));
    CHECK(c.contains(tg::pos3d(0.5, 0, 4.5)));
    CHECK(tgtest::approx(tg::pos3d(0.5, 0, 2).signed_distance_to(c), -0.5));
    CHECK(tgtest::approx(tg::pos3d(0.5, 0, 2).project_to(c.boundary()), tg::pos3d(1, 0, 2)));

    auto const down = tg::ray3d(tg::pos3d(0, 0, 10), tg::vec3d(0, 0, -1));
    CHECK(tgtest::approx(down.closest_intersection_parameter_with(c).value(), 5.0));
    auto const across = tg::line3d(tg::pos3d(-5, 0, 2), tg::vec3d(1, 0, 0)).intersection_parameter_with(c.boundary());
    REQUIRE(across.size() == 2);
    CHECK(tgtest::approx(across.last(), 6.0));

    // two capsules are as far apart as their axes, less both radii — an exact oracle for GJK
    auto rng = cc::random(29);
    for (auto i = 0; i < 100; ++i)
    {
        auto const rp = [&] { return tg::pos3d(rng.uniform(-4.0, 4.0), rng.uniform(-4.0, 4.0), rng.uniform(-4.0, 4.0)); };
        auto const a = tg::capsule3d(tg::segment3d(rp(), rp()), rng.uniform(0.1, 1.0));
        auto const b = tg::capsule3d(tg::segment3d(rp(), rp()), rng.uniform(0.1, 1.0));
        auto const expected = cc::max(0.0, a.axis.distance_to(b.axis) - a.radius - b.radius);
        // GJK on two curved supports converges to its tolerance, not to the closed form's last digit
        CHECK(tgtest::approx(a.distance_to(b), expected, 1e-5));
        CHECK(b.contains(b.sample_uniform(rng)));
        CHECK(tgtest::approx(b.boundary().sample_uniform(rng).distance_to(b.axis), b.radius, 1e-9));
    }
}

TEST("tg round - cylinder, its surface and its tube")
{
    auto const c = tg::cylinder3d(axis, 1.0);
    auto const pi = tg::pi<double>;

    CHECK(tgtest::approx(c.volume(), 4 * pi, 1e-9));
    CHECK(tgtest::approx(c.area(), 8 * pi + 2 * pi, 1e-9));
    CHECK(tgtest::approx(c.mantle().area(), 8 * pi, 1e-9));
    CHECK(c.bounds() == tg::aabb3d(tg::pos3d(-1, -1, 0), tg::pos3d(1, 1, 4)));
    CHECK(tgtest::approx(c.caps()[1].center, tg::pos3d(0, 0, 4)));

    SECTION("projection onto the solid, the surface and the tube")
    {
        auto const inside = tg::pos3d(0.5, 0, 3.8);
        CHECK(inside.project_to(c) == inside);
        // the near cap for the surface, the tube for the mantle, which has no caps
        CHECK(tgtest::approx(inside.project_to(c.boundary()), tg::pos3d(0.5, 0, 4)));
        CHECK(tgtest::approx(inside.project_to(c.mantle()), tg::pos3d(1, 0, 3.8)));
        CHECK(tgtest::approx(tg::pos3d(3, 0, 6).project_to(c), tg::pos3d(1, 0, 4)));
        CHECK(tgtest::approx(tg::pos3d(3, 0, 6).distance_to(c.mantle()), tg::sqrt(8.0)));
    }

    SECTION("a ray through the caps, and through the open tube")
    {
        auto const down = tg::ray3d(tg::pos3d(0.5, 0, 10), tg::vec3d(0, 0, -1));
        CHECK(tgtest::approx(down.closest_intersection_parameter_with(c).value(), 6.0));
        CHECK(tgtest::approx(down.closest_intersection_parameter_with(c.boundary()).value(), 6.0));
        // it slides down inside the tube without ever crossing it
        CHECK(!down.intersects(c.mantle()));

        auto const across = tg::line3d(tg::pos3d(-5, 0, 2), tg::vec3d(1, 0, 0));
        CHECK(across.intersection_parameter_with(c.mantle()).size() == 2);
    }

    SECTION("samples lie where they should")
    {
        auto rng = cc::random(31);
        for (auto i = 0; i < 300; ++i)
        {
            CHECK(c.contains(c.sample_uniform(rng)));
            CHECK(tgtest::approx(c.mantle().sample_uniform(rng).distance_to(c.mantle()), 0.0, 1e-9));
            CHECK(tgtest::approx(c.boundary().sample_uniform(rng).distance_to(c.boundary()), 0.0, 1e-9));
        }
    }

    SECTION("GJK agrees with the closed projection")
    {
        auto const p = tg::pos3d(3, 1, 6);
        auto const g = tg::impl::gjk(p, c);
        CHECK(tgtest::approx(g.on_a.distance_to(g.on_b), p.distance_to(c), 1e-6));
    }
}
