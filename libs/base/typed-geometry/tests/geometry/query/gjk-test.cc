#include "../../approx.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The GJK floor against closed forms on random convex pairs, and EPA's depth on overlaps whose depth is known.
// The pairs here have no closed-form kernel, so every query below reaches GJK through the ladder.

namespace
{
static_assert(tg::has_distance_sqr_to<tg::sphere3d, tg::aabb3d>);
static_assert(tg::has_intersects<tg::triangle3d, tg::segment3d>);
static_assert(!tg::has_distance_sqr_to<tg::aabb3i, tg::triangle3i>, "GJK iterates to a tolerance, so ints are refused");
static_assert(tg::has_separation_from<tg::sphere3d, tg::aabb3d>);
static_assert(!tg::has_separation_from<tg::triangle3d, tg::aabb3d>, "EPA needs two full-dimensional solids");

tg::pos3d random_pos(cc::random& rng, double extent)
{
    return tg::pos3d(rng.uniform(-extent, extent), rng.uniform(-extent, extent), rng.uniform(-extent, extent));
}

tg::aabb3d random_aabb(cc::random& rng)
{
    auto const c = random_pos(rng, 4.0);
    auto const h = tg::vec3d(rng.uniform(0.1, 2.0), rng.uniform(0.1, 2.0), rng.uniform(0.1, 2.0));
    return tg::aabb3d(c - h, c + h);
}

/// per-axis gap between two boxes, the exact distance
double aabb_distance(tg::aabb3d const& a, tg::aabb3d const& b)
{
    auto d2 = 0.0;
    for (auto i = 0; i < 3; ++i)
    {
        auto const gap = cc::max(a.min.data[i] - b.max.data[i], b.min.data[i] - a.max.data[i]);
        if (gap > 0)
            d2 += gap * gap;
    }
    return tg::sqrt(d2);
}
} // namespace

TEST("tg gjk - distances match the closed forms")
{
    auto rng = cc::random(7);

    for (auto i = 0; i < 300; ++i)
    {
        auto const a = random_aabb(rng);
        auto const b = random_aabb(rng);
        auto const s = tg::sphere3d(random_pos(rng, 4.0), rng.uniform(0.1, 2.0));
        auto const t = tg::sphere3d(random_pos(rng, 4.0), rng.uniform(0.1, 2.0));

        auto const ab = aabb_distance(a, b);
        CHECK(tgtest::approx(a.distance_to(b), ab, 1e-6));
        CHECK(a.intersects(b) == (ab == 0.0));

        auto const st = cc::max(0.0, (s.center - t.center).length() - s.radius - t.radius);
        CHECK(tgtest::approx(s.distance_to(t), st, 1e-6));

        // sphere to box through the projection of the center, which is exact
        auto const sa = cc::max(0.0, s.center.distance_to(a) - s.radius);
        CHECK(tgtest::approx(s.distance_to(a), sa, 1e-6));
    }
}

TEST("tg gjk - closest points lie on their objects and realize the distance")
{
    auto rng = cc::random(11);

    for (auto i = 0; i < 200; ++i)
    {
        auto const b = random_aabb(rng);
        auto const tri = tg::triangle3d(random_pos(rng, 5.0), random_pos(rng, 5.0), random_pos(rng, 5.0));

        auto const [on_tri, on_b] = tri.closest_points_to(b);
        auto const d = tri.distance_to(b);

        CHECK(tgtest::approx(on_tri.distance_to(on_b), d, 1e-6));
        CHECK(on_tri.distance_to(tri) < 1e-6);
        CHECK(b.contains(on_b.project_to(b)));
        CHECK(on_b.distance_to(b) < 1e-9);

        // dense sampling of the triangle only ever finds a farther point
        auto best = 1e30;
        for (auto u = 0; u <= 40; ++u)
            for (auto v = 0; u + v <= 40; ++v)
            {
                auto const p = tri.pos0 + (tri.pos1 - tri.pos0) * (u / 40.0) + (tri.pos2 - tri.pos0) * (v / 40.0);
                best = cc::min(best, p.distance_to(b));
            }
        CHECK(d <= best + 1e-9);
        CHECK(best - d < 0.5);
    }
}

TEST("tg epa - depth and direction of known overlaps")
{
    SECTION("two balls: the depth is the overlap of their radii, along the line of centers")
    {
        auto const a = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
        auto const b = tg::sphere3d(tg::pos3d(1.5, 0, 0), 1.0);
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.5, 1e-3));
        CHECK(tgtest::approx(s.value().normal, tg::vec3d(1, 0, 0), 1e-3));
    }

    SECTION("two boxes: the axis of least overlap")
    {
        auto const a = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 2, 2));
        auto const b = tg::aabb3d(tg::pos3d(1.75, 0.5, 0.5), tg::pos3d(3, 1.5, 1.5));
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.25, 1e-6));
        CHECK(tgtest::approx(s.value().normal, tg::vec3d(1, 0, 0), 1e-6));
    }

    SECTION("moving b by the separation leaves the solids touching, not overlapping")
    {
        auto const a = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 2, 2));
        auto const b = tg::sphere3d(tg::pos3d(1, 1, 2.5), 1.0);
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.5, 1e-3));
        auto const moved = tg::sphere3d(b.center + s.value().normal * s.value().depth, b.radius);
        CHECK(a.distance_to(moved) < 1e-3);
    }

    SECTION("in 2D")
    {
        auto const a = tg::sphere2d(tg::pos2d(0, 0), 1.0);
        auto const b = tg::aabb2d(tg::pos2d(0.75, -1), tg::pos2d(3, 1));
        auto const s = a.separation_from(b);
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().depth, 0.25, 1e-3));
        CHECK(tgtest::approx(s.value().normal, tg::vec2d(1, 0), 1e-3));
    }

    SECTION("apart: no separation is needed")
    {
        auto const a = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
        auto const b = tg::sphere3d(tg::pos3d(3, 0, 0), 1.0);
        CHECK(!a.separation_from(b).has_value());
    }
}
