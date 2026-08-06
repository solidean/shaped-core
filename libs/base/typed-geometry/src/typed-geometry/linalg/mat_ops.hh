#pragma once

#include <clean-core/common/macros.hh>
#include <typed-geometry/linalg/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/scalar/traits.hh>

namespace tg
{
/// the R x C matrix whose columns are this matrix's rows.
template <int C, int R, class T>
[[nodiscard]] constexpr mat<R, C, T> transpose(mat<C, R, T> const& m)
{
    mat<R, C, T> t;
    for (int c = 0; c < C; ++c)
        for (int r = 0; r < R; ++r)
            t[r, c] = m[c, r];
    return t;
}

/// the determinant of a square matrix.
template <int N, class T>
[[nodiscard]] constexpr T determinant(mat<N, N, T> const& m)
{
    static_assert(N <= 4, "typed-geometry is 2D/3D, so a determinant is only defined up to the 4x4 homogeneous case");

    if constexpr (N == 1)
    {
        return m[0, 0];
    }
    else if constexpr (N == 2)
    {
        return m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
    }
    else if constexpr (N == 3)
    {
        // expansion along row 0; cij is the 2x2 minor over rows 1 and 2 of columns i and j
        T const c12 = m[1, 1] * m[2, 2] - m[2, 1] * m[1, 2];
        T const c02 = m[0, 1] * m[2, 2] - m[2, 1] * m[0, 2];
        T const c01 = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
        return m[0, 0] * c12 - m[1, 0] * c02 + m[2, 0] * c01;
    }
    else
    {
        // lij / rij are the 2x2 minors over rows i and j, taken from columns 0 and 1 / columns 2 and 3
        T const l01 = m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
        T const l02 = m[0, 0] * m[1, 2] - m[1, 0] * m[0, 2];
        T const l03 = m[0, 0] * m[1, 3] - m[1, 0] * m[0, 3];
        T const l12 = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
        T const l13 = m[0, 1] * m[1, 3] - m[1, 1] * m[0, 3];
        T const l23 = m[0, 2] * m[1, 3] - m[1, 2] * m[0, 3];

        T const r01 = m[2, 0] * m[3, 1] - m[3, 0] * m[2, 1];
        T const r02 = m[2, 0] * m[3, 2] - m[3, 0] * m[2, 2];
        T const r03 = m[2, 0] * m[3, 3] - m[3, 0] * m[2, 3];
        T const r12 = m[2, 1] * m[3, 2] - m[3, 1] * m[2, 2];
        T const r13 = m[2, 1] * m[3, 3] - m[3, 1] * m[2, 3];
        T const r23 = m[2, 2] * m[3, 3] - m[3, 2] * m[2, 3];

        return l01 * r23 - l02 * r13 + l03 * r12 + l12 * r03 - l13 * r02 + l23 * r01;
    }
}

/// the adjugate: the transposed cofactor matrix, satisfying adjugate(m) * m == determinant(m) * identity.
/// Defined for singular matrices too, which is what makes it the division-free building block.
template <int N, class T>
[[nodiscard]] CC_FORCE_INLINE constexpr mat<N, N, T> adjugate(mat<N, N, T> const& m)
{
    static_assert(N <= 4, "typed-geometry is 2D/3D, so an adjugate is only defined up to the 4x4 homogeneous case");

    mat<N, N, T> a;

    if constexpr (N == 1)
    {
        a[0, 0] = tg::one<T>();
    }
    else if constexpr (N == 2)
    {
        a[0, 0] = m[1, 1];
        a[1, 0] = -m[1, 0];
        a[0, 1] = -m[0, 1];
        a[1, 1] = m[0, 0];
    }
    else if constexpr (N == 3)
    {
        // row r holds the cross product of the two columns other than r
        a[0, 0] = m[1, 1] * m[2, 2] - m[2, 1] * m[1, 2];
        a[1, 0] = m[2, 0] * m[1, 2] - m[1, 0] * m[2, 2];
        a[2, 0] = m[1, 0] * m[2, 1] - m[2, 0] * m[1, 1];

        a[0, 1] = m[2, 1] * m[0, 2] - m[0, 1] * m[2, 2];
        a[1, 1] = m[0, 0] * m[2, 2] - m[2, 0] * m[0, 2];
        a[2, 1] = m[2, 0] * m[0, 1] - m[0, 0] * m[2, 1];

        a[0, 2] = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
        a[1, 2] = m[1, 0] * m[0, 2] - m[0, 0] * m[1, 2];
        a[2, 2] = m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
    }
    else
    {
        // lij / rij are the 2x2 minors over rows i and j, taken from columns 0 and 1 / columns 2 and 3
        T const l01 = m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
        T const l02 = m[0, 0] * m[1, 2] - m[1, 0] * m[0, 2];
        T const l03 = m[0, 0] * m[1, 3] - m[1, 0] * m[0, 3];
        T const l12 = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
        T const l13 = m[0, 1] * m[1, 3] - m[1, 1] * m[0, 3];
        T const l23 = m[0, 2] * m[1, 3] - m[1, 2] * m[0, 3];

        T const r01 = m[2, 0] * m[3, 1] - m[3, 0] * m[2, 1];
        T const r02 = m[2, 0] * m[3, 2] - m[3, 0] * m[2, 2];
        T const r03 = m[2, 0] * m[3, 3] - m[3, 0] * m[2, 3];
        T const r12 = m[2, 1] * m[3, 2] - m[3, 1] * m[2, 2];
        T const r13 = m[2, 1] * m[3, 3] - m[3, 1] * m[2, 3];
        T const r23 = m[2, 2] * m[3, 3] - m[3, 2] * m[2, 3];

        // a 3x3 minor drops one column, so it expands along that pair's leftover column against the other pair's minors
        a[0, 0] = m[1, 1] * r23 - m[1, 2] * r13 + m[1, 3] * r12;
        a[1, 0] = m[1, 2] * r03 - m[1, 0] * r23 - m[1, 3] * r02;
        a[2, 0] = m[1, 0] * r13 - m[1, 1] * r03 + m[1, 3] * r01;
        a[3, 0] = m[1, 1] * r02 - m[1, 0] * r12 - m[1, 2] * r01;

        a[0, 1] = m[0, 2] * r13 - m[0, 1] * r23 - m[0, 3] * r12;
        a[1, 1] = m[0, 0] * r23 - m[0, 2] * r03 + m[0, 3] * r02;
        a[2, 1] = m[0, 1] * r03 - m[0, 0] * r13 - m[0, 3] * r01;
        a[3, 1] = m[0, 0] * r12 - m[0, 1] * r02 + m[0, 2] * r01;

        a[0, 2] = m[3, 1] * l23 - m[3, 2] * l13 + m[3, 3] * l12;
        a[1, 2] = m[3, 2] * l03 - m[3, 0] * l23 - m[3, 3] * l02;
        a[2, 2] = m[3, 0] * l13 - m[3, 1] * l03 + m[3, 3] * l01;
        a[3, 2] = m[3, 1] * l02 - m[3, 0] * l12 - m[3, 2] * l01;

        a[0, 3] = m[2, 2] * l13 - m[2, 1] * l23 - m[2, 3] * l12;
        a[1, 3] = m[2, 0] * l23 - m[2, 2] * l03 + m[2, 3] * l02;
        a[2, 3] = m[2, 1] * l03 - m[2, 0] * l13 - m[2, 3] * l01;
        a[3, 3] = m[2, 0] * l12 - m[2, 1] * l02 + m[2, 2] * l01;
    }

    return a;
}

/// the cofactor matrix, equal to determinant(m) * transpose(inverse(m)) but without the division.
///
/// This is what a normal transforms by: a normal is a covector, so it picks up the inverse transpose.
template <int N, class T>
[[nodiscard]] constexpr mat<N, N, T> cofactor(mat<N, N, T> const& m)
{
    return tg::transpose(tg::adjugate(m));
}

/// the inverse of a square matrix.
/// A singular matrix has no inverse and yields the zero matrix — check the determinant if you must tell the two apart.
template <int N, class T>
[[nodiscard]] constexpr mat<N, N, T> inverse(mat<N, N, T> const& m)
{
    auto const a = tg::adjugate(m);

    // row 0 expanded against the adjugate's cofactors is determinant(m), so the minors are only computed once
    T d = m[0, 0] * a[0, 0];
    for (int c = 1; c < N; ++c)
        d += m[c, 0] * a[0, c];

    if (tg::traits::is_zero(d))
        return {};

    return a * (tg::one<T>() / d);
}

} // namespace tg
