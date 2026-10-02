#include "../approx.hh"

#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The tetrahedron and its faces, the bilinear quad, and the ellipsoid in the GJK floor.

namespace
{
static_assert(tg::has_separation_from<tg::tetrahedron3d, tg::ellipsoid3d>, "both have a support");
static_assert(!tg::has_distance_sqr_to<tg::quad3d, tg::aabb3d>, "a bilinear patch is not convex");
static_assert(tg::has_intersection_parameter_with<tg::ray3d, tg::quad3d>);

auto const corner = tg::tetrahedron3d(tg::pos3d(0, 0, 0), tg::pos3d(1, 0, 0), tg::pos3d(0, 1, 0), tg::pos3d(0, 0, 1));
} // namespace

TEST("tg polytope - tetrahedron")
{
    CHECK(tgtest::approx(corner.volume(), 1.0 / 6.0));
    CHECK(tgtest::approx(corner.area(), 1.5 + tg::sqrt(3.0) / 2, 1e-12));
    CHECK(tgtest::approx(corner.centroid(), tg::pos3d(0.25, 0.25, 0.25)));
    CHECK(corner.edges()[5] == tg::segment3d(corner.pos2, corner.pos3));

    auto const b = corner.parameter_of(tg::pos3d(0.1, 0.2, 0.3));
    CHECK(tgtest::approx(b.data[0], 0.4));
    CHECK(tgtest::approx(corner.at(b), tg::pos3d(0.1, 0.2, 0.3)));

    CHECK(corner.contains(tg::pos3d(0.1, 0.1, 0.1)));
    CHECK(!corner.contains(tg::pos3d(0.5, 0.5, 0.5)));
    CHECK(tgtest::approx(tg::pos3d(-1, 0.2, 0.2).distance_to(corner), 1.0));
    CHECK(tgtest::approx(tg::pos3d(0.1, 0.2, 0.2).project_to(corner.boundary()), tg::pos3d(0, 0.2, 0.2)));

    auto const r = tg::ray3d(tg::pos3d(0.1, 0.1, -2), tg::vec3d(0, 0, 1));
    auto const in = r.intersection_parameter_with(corner).value();
    CHECK(tgtest::approx(in.start, 2.0));
    CHECK(tgtest::approx(in.end, 2.8));
    CHECK(r.intersection_parameter_with(corner.boundary()).size() == 2);

    auto rng = cc::random(43);
    for (auto i = 0; i < 300; ++i)
    {
        CHECK(corner.contains(corner.sample_uniform(rng)));
        CHECK(corner.boundary().sample_uniform(rng).distance_to(corner.boundary()) < 1e-12);

        auto const p = tg::pos3d(rng.uniform(-2.0, 2.0), rng.uniform(-2.0, 2.0), rng.uniform(-2.0, 2.0));
        auto const g = tg::impl::gjk(p, corner);
        CHECK(tgtest::approx(p.distance_to(corner), g.on_a.distance_to(g.on_b), 1e-6));
    }
}

TEST("tg polytope - the sample mean of a tetrahedron is its centroid")
{
    auto rng = cc::random(47);
    auto sum = tg::vec3d();
    for (auto i = 0; i < 20000; ++i)
        sum += corner.sample_uniform(rng) - tg::pos3d();
    CHECK(tgtest::approx(tg::pos3d() + sum / 20000.0, corner.centroid(), 0.01));
}

TEST("tg polytope - a bilinear quad")
{
    // a saddle: the corners at heights 0, 1, 0, 1 around the unit square
    auto const q = tg::quad3d(tg::pos3d(0, 0, 0), tg::pos3d(1, 0, 1), tg::pos3d(1, 1, 0), tg::pos3d(0, 1, 1));
    CHECK(tgtest::approx(q.at(tg::comp2d(0.5, 0.5)), tg::pos3d(0.5, 0.5, 0.5)));
    CHECK(q.bounds() == tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(1, 1, 1)));

    // straight down through (u, v) = (0.25, 0.5): the patch height there is u + v - 2uv = 0.5
    auto const r = tg::ray3d(tg::pos3d(0.25, 0.5, 5), tg::vec3d(0, 0, -1));
    auto const h = r.intersection_parameter_with(q);
    REQUIRE(h.size() == 1);
    CHECK(tgtest::approx(h.first(), 4.5, 1e-9));
    CHECK(!tg::ray3d(tg::pos3d(2, 2, 5), tg::vec3d(0, 0, -1)).intersects(q));

    // a flat quad is a parallelogram
    auto const flat = tg::quad3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 0, 0), tg::pos3d(2, 1, 0), tg::pos3d(0, 1, 0));
    CHECK(tgtest::approx(
        tg::ray3d(tg::pos3d(1, 0.5, 3), tg::vec3d(0, 0, -1)).closest_intersection_parameter_with(flat).value(), 3.0));
}

TEST("tg polytope - an ellipsoid in the GJK floor")
{
    auto const e = tg::ellipsoid3d(tg::pos3d(0, 0, 0), tg::vec3d(3, 0, 0), tg::vec3d(0, 1, 0), tg::vec3d(0, 0, 1));
    CHECK(tgtest::approx(tg::pos3d(5, 0, 0).distance_to(e), 2.0, 1e-6));
    CHECK(e.contains(tg::pos3d(2.5, 0, 0)));
    CHECK(!e.contains(tg::pos3d(0, 1.5, 0)));
    CHECK(e.intersects(tg::sphere3d(tg::pos3d(0, 2.5, 0), 1.6)));
    CHECK(!e.intersects(tg::sphere3d(tg::pos3d(0, 2.5, 0), 1.4)));

    auto const s = e.separation_from(tg::sphere3d(tg::pos3d(3.5, 0, 0), 1.0)).value();
    CHECK(tgtest::approx(s.depth, 0.5, 1e-3));
}
