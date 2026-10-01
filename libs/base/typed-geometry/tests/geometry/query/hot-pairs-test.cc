#include "../../approx.hh"

#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// Each closed-form hot pair against the GJK floor it replaced, on random inputs, plus the epsilon overloads.

namespace
{
tg::pos3d random_pos(cc::random& rng, double extent)
{
    return tg::pos3d(rng.uniform(-extent, extent), rng.uniform(-extent, extent), rng.uniform(-extent, extent));
}

tg::aabb3d random_aabb(cc::random& rng)
{
    auto const c = random_pos(rng, 3.0);
    auto const h = tg::vec3d(rng.uniform(0.1, 1.5), rng.uniform(0.1, 1.5), rng.uniform(0.1, 1.5));
    return tg::aabb3d(c - h, c + h);
}

tg::box3d random_box(cc::random& rng)
{
    auto const h = tg::mat3d::make_from_cols(random_pos(rng, 1.0) - tg::pos3d(), random_pos(rng, 1.0) - tg::pos3d(),
                                             random_pos(rng, 1.0) - tg::pos3d());
    return tg::box3d(random_pos(rng, 3.0), h);
}
} // namespace

TEST("tg hot pairs - closed forms agree with GJK")
{
    auto rng = cc::random(17);

    for (auto i = 0; i < 300; ++i)
    {
        auto const a = random_aabb(rng);
        auto const b = random_aabb(rng);
        auto const s = tg::sphere3d(random_pos(rng, 3.0), rng.uniform(0.1, 2.0));
        auto const t = tg::sphere3d(random_pos(rng, 3.0), rng.uniform(0.1, 2.0));
        auto const p = tg::segment3d(random_pos(rng, 4.0), random_pos(rng, 4.0));
        auto const q = tg::segment3d(random_pos(rng, 4.0), random_pos(rng, 4.0));

        auto const gab = tg::impl::gjk(a, b);
        CHECK(a.intersects(b) == gab.overlapping);
        CHECK(tgtest::approx(a.distance_to(b), gab.on_a.distance_to(gab.on_b), 1e-6));

        auto const gst = tg::impl::gjk(s, t);
        CHECK(s.intersects(t) == gst.overlapping);
        CHECK(tgtest::approx(s.distance_to(t), gst.on_a.distance_to(gst.on_b), 1e-6));

        auto const gsa = tg::impl::gjk(s, a);
        CHECK(s.intersects(a) == gsa.overlapping);
        CHECK(tgtest::approx(s.distance_to(a), gsa.on_a.distance_to(gsa.on_b), 1e-6));

        auto const gpq = tg::impl::gjk(p, q);
        auto const [on_p, on_q] = p.closest_points_to(q);
        CHECK(tgtest::approx(on_p.distance_to(on_q), gpq.on_a.distance_to(gpq.on_b), 1e-6));

        auto const ba = random_box(rng);
        auto const bb = random_box(rng);
        CHECK(ba.intersects(bb) == tg::impl::gjk(ba, bb).overlapping);
    }
}

TEST("tg hot pairs - separations along the obvious axis")
{
    auto const a = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
    auto const b = tg::sphere3d(tg::pos3d(0, 1.5, 0), 1.0);
    auto const s = a.separation_from(b).value();
    CHECK(tgtest::approx(s.depth, 0.5));
    CHECK(tgtest::approx(s.normal, tg::vec3d(0, 1, 0)));

    auto const x = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(2, 2, 2));
    auto const y = tg::aabb3d(tg::pos3d(-0.5, 0.5, 0.5), tg::pos3d(0.25, 1.5, 1.5));
    auto const sep = x.separation_from(y).value();
    CHECK(tgtest::approx(sep.depth, 0.25));
    CHECK(sep.normal == tg::vec3d(-1, 0, 0));
}

TEST("tg hot pairs - parallel segments still give a nearest pair")
{
    auto const p = tg::segment3d(tg::pos3d(0, 0, 0), tg::pos3d(4, 0, 0));
    auto const q = tg::segment3d(tg::pos3d(1, 2, 0), tg::pos3d(3, 2, 0));
    auto const [on_p, on_q] = p.closest_points_to(q);
    CHECK(tgtest::approx(on_p.distance_to(on_q), 2.0));
}

TEST("tg hot pairs - the epsilon overloads")
{
    auto const a = tg::aabb3d(tg::pos3d(0, 0, 0), tg::pos3d(1, 1, 1));
    auto const b = tg::aabb3d(tg::pos3d(1.05, 0, 0), tg::pos3d(2, 1, 1));

    CHECK(!a.intersects(b));
    CHECK(a.intersects(b, 0.1));
    CHECK(!a.intersects(b, 0.01));

    CHECK(tg::sphere3d(tg::pos3d(0, 0, 0), 1.0).contains(tg::pos3d(1.05, 0, 0), 0.1));
    CHECK(!tg::sphere3d(tg::pos3d(0, 0, 0), 1.0).contains(tg::pos3d(1.5, 0, 0), 0.1));
}
