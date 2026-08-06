#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::mat3f>, "mat should be trivially copyable");

TEST("tg mat - identity and access")
{
    SECTION("default is zero, not identity")
    {
        tg::mat3f m;
        CHECK(m == tg::mat3f::zero);
        CHECK((m[0, 0]) == 0);
    }

    SECTION("identity has ones on the diagonal")
    {
        auto const id = tg::mat3f::identity;
        CHECK((id[0, 0]) == 1);
        CHECK((id[1, 1]) == 1);
        CHECK((id[2, 2]) == 1);
        CHECK((id[1, 0]) == 0);
        CHECK((id[0, 1]) == 0);
    }

    SECTION("col() returns column vectors")
    {
        auto const id = tg::mat3f::identity;
        CHECK(id.col(0) == tg::vec3f(1, 0, 0));
        CHECK(id.col(2) == tg::vec3f(0, 0, 1));
    }

    SECTION("out-of-range asserts")
    {
        tg::mat3f m;
        CHECK_ASSERTS((m[3, 0]));
        CHECK_ASSERTS((m[0, 3]));
    }
}

TEST("tg mat - products")
{
    auto const id = tg::mat3f::identity;
    auto const v = tg::vec3f(2, 3, 4);

    SECTION("identity is neutral")
    {
        CHECK(id * v == v);
        CHECK(id * id == id);
    }

    SECTION("make_from_cols")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
        CHECK(m == id);
    }
}

TEST("tg mat - rotations")
{
    auto const quarter = tg::angle_f::make_from_degree(90);
    auto const ex = tg::vec3f(1, 0, 0);
    auto const ey = tg::vec3f(0, 1, 0);
    auto const ez = tg::vec3f(0, 0, 1);

    SECTION("rotation_z maps x -> y")
    {
        auto const r = tg::mat3f::make_rotation_z(quarter);
        CHECK(tgtest::approx(r * ex, ey));
    }

    SECTION("rotation_x maps y -> z")
    {
        auto const r = tg::mat3f::make_rotation_x(quarter);
        CHECK(tgtest::approx(r * ey, ez));
    }

    SECTION("rotation_y maps z -> x")
    {
        auto const r = tg::mat3f::make_rotation_y(quarter);
        CHECK(tgtest::approx(r * ez, ex));
    }

    SECTION("axis-angle about z equals rotation_z")
    {
        auto const r1 = tg::mat3f::make_rotation_axis_angle(ez, quarter);
        auto const r2 = tg::mat3f::make_rotation_z(quarter);
        CHECK(tgtest::approx(r1 * ex, r2 * ex));
        CHECK(tgtest::approx(r1 * ey, r2 * ey));
    }
}

namespace
{
tg::mat3f sample3()
{
    return tg::mat3f::make_from_cols(tg::vec3f(1, 2, 3), tg::vec3f(0, 1, 4), tg::vec3f(5, 6, 0));
}

tg::mat4f sample4()
{
    return tg::mat4f::make_from_cols(tg::vec4f(1, 0, 2, 1), tg::vec4f(0, 3, 1, 0), tg::vec4f(2, 1, 0, 1),
                                     tg::vec4f(1, 1, 0, 2));
}
} // namespace

TEST("tg mat - transposed")
{
    SECTION("rectangular transpose swaps the shape")
    {
        auto const m = tg::mat<3, 2, float>::make_from_cols(tg::vec2f(1, 2), tg::vec2f(3, 4), tg::vec2f(5, 6));
        auto const t = m.transposed();

        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 2; ++r)
                CHECK((m[c, r]) == (t[r, c]));
    }

    SECTION("transposed is an involution")
    {
        auto const m = sample3();
        CHECK(m.transposed().transposed() == m);
    }

    SECTION("the identity is symmetric")
    {
        CHECK(tg::mat3f::identity.transposed() == tg::mat3f::identity);
    }
}

TEST("tg mat - determinant")
{
    SECTION("identity has determinant one")
    {
        CHECK(tg::mat2f::identity.determinant() == 1);
        CHECK(tg::mat3f::identity.determinant() == 1);
        CHECK(tg::mat4f::identity.determinant() == 1);
    }

    SECTION("2x2 by hand")
    {
        auto const m = tg::mat2f::make_from_cols(tg::vec2f(1, 3), tg::vec2f(2, 4));
        CHECK(tgtest::approx(m.determinant(), 1.0f * 4 - 2 * 3));
    }

    SECTION("3x3 by hand")
    {
        CHECK(tgtest::approx(sample3().determinant(), 1.0f));
    }

    SECTION("4x4 by hand")
    {
        CHECK(tgtest::approx(sample4().determinant(), -20.0f));
    }

    SECTION("the determinant is multiplicative")
    {
        auto const a3 = sample3();
        auto const b3 = tg::mat3f::make_from_cols(tg::vec3f(2, 1, 0), tg::vec3f(0, 3, 1), tg::vec3f(1, 0, 2));
        CHECK(tgtest::approx((a3 * b3).determinant(), a3.determinant() * b3.determinant()));

        auto const a4 = sample4();
        auto const b4 = tg::mat4f::make_from_cols(tg::vec4f(0, 1, 1, 2), tg::vec4f(3, 0, 1, 0), tg::vec4f(1, 2, 0, 1),
                                                  tg::vec4f(0, 1, 3, 1));
        CHECK(tgtest::approx((a4 * b4).determinant(), a4.determinant() * b4.determinant(), 1e-3f));
    }

    SECTION("the determinant is invariant under transpose")
    {
        CHECK(tgtest::approx(sample3().transposed().determinant(), sample3().determinant()));
        CHECK(tgtest::approx(sample4().transposed().determinant(), sample4().determinant()));
    }

    SECTION("a rotation preserves volume")
    {
        auto const r = tg::mat3f::make_rotation_z(tg::angle_f::make_from_degree(37));
        CHECK(tgtest::approx(r.determinant(), 1.0f));
    }

    SECTION("a scaling multiplies the determinant by the factor per dimension")
    {
        CHECK(tgtest::approx((tg::mat3f::identity * 2.0f).determinant(), 8.0f));
    }

    SECTION("a singular matrix has determinant zero")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 2, 3), tg::vec3f(2, 4, 6), tg::vec3f(0, 1, 0));
        CHECK(tgtest::approx(m.determinant(), 0.0f));
    }
}

TEST("tg mat - adjugate and cofactor")
{
    SECTION("adjugate * m == determinant * identity, even when singular")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 2, 3), tg::vec3f(2, 4, 6), tg::vec3f(0, 1, 0));
        CHECK(tgtest::approx(m.adjugate() * m, tg::mat3f::identity * m.determinant()));
    }

    SECTION("adjugate * m == determinant * identity, 2x2 and 4x4")
    {
        auto const m2 = tg::mat2f::make_from_cols(tg::vec2f(4, 1), tg::vec2f(7, 3));
        CHECK(tgtest::approx(m2.adjugate() * m2, tg::mat2f::identity * m2.determinant()));

        auto const m4 = sample4();
        CHECK(tgtest::approx(m4.adjugate() * m4, tg::mat4f::identity * m4.determinant()));
    }

    SECTION("a singular 4x4 still has an adjugate")
    {
        // the last column is the sum of the first two
        auto const m = tg::mat4f::make_from_cols(tg::vec4f(1, 0, 2, 1), tg::vec4f(0, 3, 1, 0), tg::vec4f(2, 1, 0, 1),
                                                 tg::vec4f(1, 3, 3, 1));
        CHECK(tgtest::approx(m.determinant(), 0.0f));
        CHECK(tgtest::approx(m.adjugate() * m, tg::mat4f::zero));
    }

    SECTION("cofactor is the transposed adjugate")
    {
        auto const m = sample3();
        CHECK(m.cofactor() == m.adjugate().transposed());
    }

    SECTION("cofactor equals determinant times the inverse transpose")
    {
        auto const m = sample3();
        auto const expected = m.inverse().transposed() * m.determinant();
        CHECK(tgtest::approx(m.cofactor(), expected, 1e-3f));
    }
}

TEST("tg mat - inverse")
{
    SECTION("3x3 round trip")
    {
        auto const m = sample3();
        CHECK(tgtest::approx(m.inverse() * m, tg::mat3f::identity, 1e-4f));
        CHECK(tgtest::approx(m * m.inverse(), tg::mat3f::identity, 1e-4f));
    }

    SECTION("4x4 round trip")
    {
        auto const m = sample4();
        CHECK(tgtest::approx(m.inverse() * m, tg::mat4f::identity, 1e-4f));
    }

    SECTION("2x2 round trip")
    {
        auto const m = tg::mat2f::make_from_cols(tg::vec2f(4, 1), tg::vec2f(7, 3));
        CHECK(tgtest::approx(m.inverse() * m, tg::mat2f::identity, 1e-5f));
    }

    SECTION("a rotation inverts to its transpose")
    {
        auto const r = tg::mat3f::make_rotation_y(tg::angle_f::make_from_degree(63));
        CHECK(tgtest::approx(r.inverse(), r.transposed(), 1e-5f));
    }

    SECTION("a singular matrix inverts to zero")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 2, 3), tg::vec3f(2, 4, 6), tg::vec3f(0, 1, 0));
        CHECK(m.inverse() == tg::mat3f::zero);

        auto const m4 = tg::mat4f::make_from_cols(tg::vec4f(1, 0, 2, 1), tg::vec4f(0, 3, 1, 0), tg::vec4f(2, 1, 0, 1),
                                                  tg::vec4f(1, 3, 3, 1));
        CHECK(m4.inverse() == tg::mat4f::zero);
    }

    SECTION("an all-zero matrix inverts to zero")
    {
        CHECK(tg::mat3f::zero.inverse() == tg::mat3f::zero);
    }
}
