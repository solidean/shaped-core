#include "../approx.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>
#include <typed-geometry/geometry/query/query.hh>

// sample_uniform: every sample lies in the object's point set, and simple statistics of many samples match the
// uniform distribution over it — the mean is the centroid, and a ball holds 1/8 of its samples within half its radius.

namespace
{
constexpr int sample_count = 20000;
} // namespace

TEST("tg sampling - samples lie in the object")
{
    auto rng = cc::random(3);

    auto const tri = tg::triangle3d(tg::pos3d(0, 0, 0), tg::pos3d(4, 1, 0), tg::pos3d(1, 3, 2));
    auto const box = tg::aabb3d(tg::pos3d(-1, 0, 2), tg::pos3d(3, 1, 5));
    auto const ball = tg::sphere3d(tg::pos3d(1, 2, 3), 2.0);
    auto const h = tg::mat3d::make_from_cols(tg::vec3d(1, 1, 0), tg::vec3d(0, 2, 0), tg::vec3d(0, 0, 1));
    auto const obox = tg::box3d(tg::pos3d(0, 0, 0), h);

    for (auto i = 0; i < 1000; ++i)
    {
        CHECK(tri.sample_uniform(rng).distance_to(tri) < 1e-9);
        CHECK(box.contains(box.sample_uniform(rng)));
        CHECK(ball.contains(ball.sample_uniform(rng)));
        CHECK(obox.contains(obox.sample_uniform(rng)));
        CHECK(box.boundary().sample_uniform(rng).distance_to(box.boundary()) < 1e-12);
        CHECK(tgtest::approx(ball.boundary().sample_uniform(rng).distance_to(ball.center), 2.0, 1e-9));

        auto const disk = tg::disk3d(tg::pos3d(1, 1, 1), 2.0, tg::vec3d(0, 0.6, 0.8));
        auto const dp = disk.sample_uniform(rng);
        CHECK(tg::abs(tg::dot(dp - disk.center, disk.normal)) < 1e-12);
        CHECK(dp.distance_to(disk.center) <= 2.0);
        auto const cp = disk.boundary().sample_uniform(rng);
        CHECK(tgtest::approx(cp.distance_to(disk.center), 2.0, 1e-9));
        CHECK(tg::abs(tg::dot(cp - disk.center, disk.normal)) < 1e-12);

        // on a face, the largest box coordinate is +-1
        auto const c = obox.parameter_of(obox.boundary().sample_uniform(rng));
        auto reach = 0.0;
        for (auto k = 0; k < 3; ++k)
            reach = cc::max(reach, tg::abs(c.data[k]));
        CHECK(tgtest::approx(reach, 1.0, 1e-9));
    }
}

TEST("tg sampling - the distribution is uniform")
{
    auto rng = cc::random(5);

    SECTION("the mean of a triangle's samples is its centroid")
    {
        auto const tri = tg::triangle2d(tg::pos2d(0, 0), tg::pos2d(6, 0), tg::pos2d(0, 3));
        auto sum = tg::vec2d();
        for (auto i = 0; i < sample_count; ++i)
            sum += tri.sample_uniform(rng) - tg::pos2d();
        CHECK(tgtest::approx(tg::pos2d() + sum / double(sample_count), tri.centroid(), 0.05));
    }

    SECTION("a ball holds an eighth of its samples within half its radius")
    {
        auto const ball = tg::sphere3d(tg::pos3d(0, 0, 0), 1.0);
        auto inner = 0;
        for (auto i = 0; i < sample_count; ++i)
            inner += ball.sample_uniform(rng).distance_to(ball.center) < 0.5 ? 1 : 0;
        CHECK(tgtest::approx(inner / double(sample_count), 0.125, 0.01));
    }

    SECTION("a box's faces are hit in proportion to their area")
    {
        // the box is 2 x 4 x 1: the face pairs have areas 2 * 4 (x), 2 * 2 (y) and 2 * 8 (z), of 28 in all
        auto const s = tg::aabb3d(tg::pos3d(-1, -2, -0.5), tg::pos3d(1, 2, 0.5)).boundary();
        auto on_y = 0;
        for (auto i = 0; i < sample_count; ++i)
        {
            auto const p = s.sample_uniform(rng);
            on_y += (p.data[1] == -2 || p.data[1] == 2) ? 1 : 0;
        }
        CHECK(tgtest::approx(on_y / double(sample_count), 4.0 / 28.0, 0.01));
    }
}
