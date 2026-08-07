#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>
#include <typed-geometry/transform/compose.hh>

namespace
{
/// A left-handed perspective projection in tg's column-vector convention, with z mapped into [0, 1].
/// Written out here rather than in the library: the handedness and depth range are a rendering
/// convention, and tg has not adopted one.
tg::projective_transform3f perspective(tg::angle_f vertical_fov, float aspect, float z_near, float z_far)
{
    auto const t = 1.0f / tg::tan(vertical_fov / 2.0f);

    auto m = tg::mat4f::zero;
    m[0, 0] = t / aspect;
    m[1, 1] = t;
    m[2, 2] = z_far / (z_far - z_near);
    m[3, 2] = -z_near * z_far / (z_far - z_near);
    m[2, 3] = 1.0f;
    return tg::projective_transform3f::make_from_mat(m);
}
} // namespace

TEST("tg projective - a point divides by w")
{
    auto const p = perspective(tg::angle_f::make_from_degree(90), 1.0f, 1.0f, 100.0f);

    SECTION("a point at the near plane maps to depth zero")
    {
        auto const r = (tg::pos3f(0, 0, 1)).transformed(p);
        CHECK(tgtest::approx(r, tg::pos3f(0, 0, 0), 1e-4f));
    }

    SECTION("a point at the far plane maps to depth one")
    {
        auto const r = (tg::pos3f(0, 0, 100)).transformed(p);
        CHECK(tgtest::approx(r.data[2], 1.0f, 1e-4f));
    }

    SECTION("things further away appear smaller")
    {
        // near/far would collide with the windows.h macros
        auto const closer = (tg::pos3f(1, 0, 2)).transformed(p);
        auto const further = (tg::pos3f(1, 0, 8)).transformed(p);
        CHECK(closer.data[0] > further.data[0]);
        CHECK(further.data[0] > 0.0f);
    }

    SECTION("a point on the camera plane maps to infinity and asserts")
    {
        CHECK_ASSERTS((tg::pos3f(0, 0, 0)).transformed(p));
    }

    SECTION("the inverse undoes it")
    {
        auto const q = tg::pos3f(2, -1, 7);
        CHECK(tgtest::approx((q.transformed(p)).transformed(p.inverse()), q, 1e-3f));
    }
}

TEST("tg projective - a triangle transforms vertex by vertex")
{
    auto const p = perspective(tg::angle_f::make_from_degree(60), 16.0f / 9.0f, 0.5f, 50.0f);
    auto const tri = tg::triangle3f(tg::pos3f(-1, -1, 3), tg::pos3f(1, -1, 5), tg::pos3f(0, 1, 4));

    SECTION("the result matches transforming the vertices individually")
    {
        auto const r = tri.transformed(p);
        CHECK(tgtest::approx(r.pos0, tri.pos0.transformed(p), 1e-4f));
        CHECK(tgtest::approx(r.pos1, tri.pos1.transformed(p), 1e-4f));
        CHECK(tgtest::approx(r.pos2, tri.pos2.transformed(p), 1e-4f));
    }

    SECTION("a vertex behind the projection is not diagnosed")
    {
        // w is negative there, so the vertex lands on its mirror image and the triangle is no longer the image of the source hull.
        // Nothing checks that — a caller who cares must clip first.
        auto const behind = tg::triangle3f(tg::pos3f(-1, -1, 3), tg::pos3f(1, -1, 5), tg::pos3f(0, 1, -4));
        auto const r = behind.transformed(p);
        CHECK(tgtest::approx(r.pos2, behind.pos2.transformed(p), 1e-4f));
    }

    SECTION("a segment behaves the same way")
    {
        auto const s = tg::segment3f(tg::pos3f(0, 0, 2), tg::pos3f(1, 1, 9));
        auto const r = s.transformed(p);
        CHECK(tgtest::approx(r.pos0, s.pos0.transformed(p), 1e-4f));
        CHECK(tgtest::approx(r.pos1, s.pos1.transformed(p), 1e-4f));
    }
}

TEST("tg projective - a plane stays a plane")
{
    auto const p = perspective(tg::angle_f::make_from_degree(70), 1.5f, 0.5f, 40.0f);

    SECTION("points on the source plane land on the image plane")
    {
        // the plane z == 4, well inside the frustum
        auto const source = tg::plane3f(tg::vec3f(0, 0, 1), 4.0f);
        auto const image = source.transformed(p);

        for (auto const& q : {tg::pos3f(0, 0, 4), tg::pos3f(2, -1, 4), tg::pos3f(-3, 2, 4)})
        {
            auto const v = q.transformed(p) - tg::pos3f(0, 0, 0);
            CHECK(tgtest::approx(tg::dot(image.normal, v), image.dist, 1e-3f));
        }
    }

    SECTION("a tilted plane too")
    {
        // the plane is derived from the three sample points so they are on it by construction
        auto const n = tg::normalize(tg::vec3f(1, 1, 2));
        auto const source = tg::plane3f(n, tg::dot(n, tg::vec3f(0, 0, 6)));
        auto const image = source.transformed(p);

        for (auto const& q : {tg::pos3f(0, 0, 6), tg::pos3f(2, 0, 5), tg::pos3f(0, 2, 5)})
        {
            auto const v = q.transformed(p) - tg::pos3f(0, 0, 0);
            CHECK(tgtest::approx(tg::dot(image.normal, v), image.dist, 1e-3f));
        }
    }

    SECTION("the normal comes out unit length")
    {
        auto const image = tg::plane3f(tg::vec3f(0, 0, 1), 4.0f).transformed(p);
        CHECK(tgtest::approx(image.normal.length(), 1.0f, 1e-4f));
    }
}
