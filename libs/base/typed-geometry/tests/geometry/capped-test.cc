#include "../approx.hh"

#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The capped round objects: the cone and the hemisphere, their surfaces and their mantles.

namespace
{
static_assert(tg::impl::is_boundary<tg::cone3f_surface>);
static_assert(!tg::impl::is_boundary<tg::cone3f_mantle>);
static_assert(tg::has_separation_from<tg::cone3d, tg::hemisphere3d>, "both have a support");

// apex at the origin, base of radius 1 at z = 2
auto const cone = tg::cone3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, 2), 1.0);
auto const dome = tg::hemisphere3d(tg::pos3d(0, 0, 0), 1.0, tg::vec3d(0, 0, 1));
} // namespace

TEST("tg capped - cone measures and projections")
{
    auto const pi = tg::pi<double>;
    CHECK(tgtest::approx(cone.volume(), 2 * pi / 3, 1e-9));
    CHECK(tgtest::approx(cone.area(), pi * (1 + tg::sqrt(5.0)), 1e-9));
    CHECK(tgtest::approx(cone.mantle().area(), pi * tg::sqrt(5.0), 1e-9));
    CHECK(tgtest::approx(cone.centroid(), tg::pos3d(0, 0, 1.5)));
    CHECK(cone.bounds() == tg::aabb3d(tg::pos3d(-1, -1, 0), tg::pos3d(1, 1, 2)));

    auto const inside = tg::pos3d(0, 0, 1.9);
    CHECK(inside.project_to(cone) == inside);
    CHECK(tgtest::approx(inside.project_to(cone.boundary()), tg::pos3d(0, 0, 2), 1e-12));
    CHECK(tgtest::approx(tg::pos3d(0, 0, -3).project_to(cone), tg::pos3d(0, 0, 0)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 5).project_to(cone), tg::pos3d(0, 0, 2)));
    // a point beside the slant drops perpendicularly onto it
    auto const beside = tg::pos3d(2, 0, 1);
    auto const foot = beside.project_to(cone.mantle());
    CHECK(tgtest::approx(tg::dot(beside - foot, tg::vec3d(1, 0, 2)), 0.0, 1e-12));

    auto rng = cc::random(37);
    for (auto i = 0; i < 100; ++i)
    {
        auto const p = tg::pos3d(rng.uniform(-3.0, 3.0), rng.uniform(-3.0, 3.0), rng.uniform(-2.0, 4.0));
        auto const g = tg::impl::gjk(p, cone);
        CHECK(tgtest::approx(p.distance_to(cone), g.on_a.distance_to(g.on_b), 1e-6));
        CHECK(cone.contains(cone.sample_uniform(rng)));
        CHECK(cone.mantle().sample_uniform(rng).distance_to(cone.mantle()) < 1e-9);
        CHECK(cone.boundary().sample_uniform(rng).distance_to(cone.boundary()) < 1e-9);
    }
}

TEST("tg capped - lines through a cone")
{
    auto const down = tg::ray3d(tg::pos3d(0, 0, 5), tg::vec3d(0, 0, -1));
    auto const in = down.intersection_parameter_with(cone).value();
    CHECK(tgtest::approx(in.start, 3.0));
    CHECK(tgtest::approx(in.end, 5.0));
    CHECK(down.intersection_parameter_with(cone.mantle()).size() == 1); // through the apex only; the base is not slant

    auto const across = tg::line3d(tg::pos3d(-5, 0, 1), tg::vec3d(1, 0, 0));
    auto const hits = across.intersection_parameter_with(cone.mantle());
    REQUIRE(hits.size() == 2);
    CHECK(tgtest::approx(hits.first(), 4.5));
    CHECK(tgtest::approx(hits.last(), 5.5));

    // the other nappe of the double cone is not part of the cone
    CHECK(!tg::ray3d(tg::pos3d(0, 0, -1), tg::vec3d(0, 0, -1)).intersects(cone));
}

TEST("tg capped - hemisphere")
{
    auto const pi = tg::pi<double>;
    CHECK(tgtest::approx(dome.volume(), 2 * pi / 3, 1e-9));
    CHECK(tgtest::approx(dome.centroid(), tg::pos3d(0, 0, 0.375)));
    CHECK(dome.bounds() == tg::aabb3d(tg::pos3d(-1, -1, 0), tg::pos3d(1, 1, 1)));

    CHECK(tgtest::approx(tg::pos3d(0, 0, -2).project_to(dome), tg::pos3d(0, 0, 0)));
    CHECK(tgtest::approx(tg::pos3d(3, 0, -1).project_to(dome), tg::pos3d(1, 0, 0)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 3).project_to(dome), tg::pos3d(0, 0, 1)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 0.1).project_to(dome.boundary()), tg::pos3d(0, 0, 0)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, 0.9).project_to(dome.boundary()), tg::pos3d(0, 0, 1)));
    CHECK(tgtest::approx(tg::pos3d(0, 0, -1).project_to(dome.mantle()).data[2], 0.0));

    auto const up = tg::ray3d(tg::pos3d(0.5, 0, -3), tg::vec3d(0, 0, 1));
    CHECK(tgtest::approx(up.closest_intersection_parameter_with(dome).value(), 3.0));
    CHECK(up.intersection_parameter_with(dome.mantle()).size() == 1);

    auto rng = cc::random(41);
    for (auto i = 0; i < 100; ++i)
    {
        auto const p = tg::pos3d(rng.uniform(-3.0, 3.0), rng.uniform(-3.0, 3.0), rng.uniform(-3.0, 3.0));
        auto const g = tg::impl::gjk(p, dome);
        CHECK(tgtest::approx(p.distance_to(dome), g.on_a.distance_to(g.on_b), 1e-6));
        CHECK(dome.contains(dome.sample_uniform(rng)));
        CHECK(dome.mantle().sample_uniform(rng).distance_to(dome.mantle()) < 1e-9);
    }
}

TEST("tg capped - a point on a cone's axis is inside, at every height")
{
    // the old inside test projected onto the profile triangle, whose rounding moved points on the axis
    for (auto const h : {3.0, 0.7, 10.0, 1e-3})
    {
        auto const tall = tg::cone3d(tg::pos3d(0, 0, 0), tg::vec3d(0, 0, h), 1.0);
        auto const slant = 1.0 / tg::sqrt(h * h + 1.0); // the sine of the half opening angle
        for (auto const f : {0.01, 0.003, 0.25, 0.5, 0.999})
        {
            auto const p = tg::pos3d(0, 0, f * h);
            CHECK(tall.contains(p));
            CHECK(p.project_to(tall) == p);
            // from the axis, the nearer of the slant and the base
            auto const expected = f * h * slant < h - f * h ? f * h * slant : h - f * h;
            CHECK(tgtest::approx(p.distance_to(tall.boundary()), expected, 1e-12));
        }
        CHECK(!tall.contains(tg::pos3d(0, 0, -0.01 * h)));
        CHECK(!tall.contains(tg::pos3d(0, 0, 1.01 * h)));
    }

    auto const tilted = tg::cone3d(tg::pos3d(1, 2, 3), tg::vec3d(1.1, -2.3, 0.7), 0.4);
    for (auto const f : {0.01, 0.3, 0.9})
        CHECK(tilted.contains(tilted.apex + tilted.axis * f));
}

TEST("tg capped - a negative uniform scale turns a hemisphere around")
{
    auto const h = tg::hemisphere3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1));
    auto const r = h.transformed(tg::signed_similarity_transform3f::make_uniform_scaling(-1.0f));
    CHECK(tgtest::approx(r.normal, tg::vec3f(0, 0, -1)));
    CHECK(tgtest::approx(r.radius, 1.0f));
    CHECK(r.contains(tg::pos3f(0, 0, -0.5f)));
    CHECK(!r.contains(tg::pos3f(0, 0, 0.5f)));
    auto const doubled = h.transformed(tg::signed_similarity_transform3f::make_uniform_scaling(-2.0f));
    CHECK(tgtest::approx(doubled.normal, tg::vec3f(0, 0, -1)));
    CHECK(tgtest::approx(doubled.radius, 2.0f));
}
