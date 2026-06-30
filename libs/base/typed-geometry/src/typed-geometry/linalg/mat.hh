#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace tg
{
/// Column-major matrix with C columns and R rows.
///
/// mat is a linear-algebra object, not a transform type (there is no mat * pos). It is stored as
/// C column vectors (`vec<R, T> cols[C]`), so col(i) is a real reference and matrix/vector products
/// fall out as column combinations. Element access uses the C++23 multi-argument subscript
/// `m[col, row]`.
///
/// The only constructor is the default one (all entries zero); everything else is a make_ factory.
/// In particular there is no "default is identity" — use mat::identity for that.
///
///     auto m = tg::mat3f::identity;
///     auto r = tg::mat3f::make_rotation_z(tg::angle_f::make_from_degree(90));
///     tg::vec3f const v = r * tg::vec3f(1, 0, 0);   // ~ (0, 1, 0)
template <int C, int R, class T>
struct mat
{
    static_assert(C > 0 && R > 0, "mat requires positive dimensions");

    vec<R, T> cols[C] = {};

    // construction
public:
    mat() = default;

    /// build from exactly C column vectors.
    template <class... Cols>
    [[nodiscard]] static constexpr mat make_from_cols(Cols const&... columns)
        requires(sizeof...(Cols) == C)
    {
        mat m;
        int i = 0;
        ((m.cols[i++] = columns), ...);
        return m;
    }

    // special values
public:
    /// the zero matrix. Runtime constant, not usable in constant expressions.
    static mat const zero;
    /// the identity matrix (rectangular identity if C != R). Runtime constant.
    static mat const identity;

    // rotations (3x3 only; require a scalar with trigonometry)
public:
    [[nodiscard]] static mat make_rotation_x(angle<T> a)
        requires(C == 3 && R == 3 && tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a);
        mat m;
        m[0, 0] = tg::one<T>();
        m[1, 1] = c;
        m[2, 1] = -s;
        m[1, 2] = s;
        m[2, 2] = c;
        return m;
    }

    [[nodiscard]] static mat make_rotation_y(angle<T> a)
        requires(C == 3 && R == 3 && tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a);
        mat m;
        m[0, 0] = c;
        m[2, 0] = s;
        m[1, 1] = tg::one<T>();
        m[0, 2] = -s;
        m[2, 2] = c;
        return m;
    }

    [[nodiscard]] static mat make_rotation_z(angle<T> a)
        requires(C == 3 && R == 3 && tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a);
        mat m;
        m[0, 0] = c;
        m[1, 0] = -s;
        m[0, 1] = s;
        m[1, 1] = c;
        m[2, 2] = tg::one<T>();
        return m;
    }

    /// rotation by `a` around the given axis (axis is assumed normalized). Rodrigues' formula.
    [[nodiscard]] static mat make_rotation_axis_angle(vec<3, T> const& axis, angle<T> a)
        requires(C == 3 && R == 3 && tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a);
        T const t = tg::one<T>() - c;
        T const x = axis.data[0];
        T const y = axis.data[1];
        T const z = axis.data[2];

        mat m;
        m.cols[0] = vec<3, T>(t * x * x + c, t * x * y + s * z, t * x * z - s * y);
        m.cols[1] = vec<3, T>(t * x * y - s * z, t * y * y + c, t * y * z + s * x);
        m.cols[2] = vec<3, T>(t * x * z + s * y, t * y * z - s * x, t * z * z + c);
        return m;
    }

    // access
public:
    [[nodiscard]] constexpr vec<R, T>& col(int c)
    {
        CC_ASSERT(0 <= c && c < C, "column index out of range");
        return cols[c];
    }
    [[nodiscard]] constexpr vec<R, T> const& col(int c) const
    {
        CC_ASSERT(0 <= c && c < C, "column index out of range");
        return cols[c];
    }

    [[nodiscard]] constexpr T& operator[](int c, int r)
    {
        CC_ASSERT(0 <= c && c < C, "column index out of range");
        CC_ASSERT(0 <= r && r < R, "row index out of range");
        return cols[c].data[r];
    }
    [[nodiscard]] constexpr T const& operator[](int c, int r) const
    {
        CC_ASSERT(0 <= c && c < C, "column index out of range");
        CC_ASSERT(0 <= r && r < R, "row index out of range");
        return cols[c].data[r];
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(mat const&, mat const&) = default;

    // arithmetic
public:
    [[nodiscard]] friend constexpr mat operator+(mat a, mat const& b)
    {
        for (int c = 0; c < C; ++c)
            a.cols[c] += b.cols[c];
        return a;
    }
    [[nodiscard]] friend constexpr mat operator-(mat a, mat const& b)
    {
        for (int c = 0; c < C; ++c)
            a.cols[c] -= b.cols[c];
        return a;
    }
    [[nodiscard]] friend constexpr mat operator*(mat a, T s)
    {
        for (int c = 0; c < C; ++c)
            a.cols[c] *= s;
        return a;
    }
    [[nodiscard]] friend constexpr mat operator*(T s, mat a)
    {
        for (int c = 0; c < C; ++c)
            a.cols[c] *= s;
        return a;
    }

    /// matrix * vector: a linear combination of the columns.
    [[nodiscard]] friend constexpr vec<R, T> operator*(mat const& m, vec<C, T> const& v)
    {
        vec<R, T> r = m.cols[0] * v.data[0];
        for (int c = 1; c < C; ++c)
            r += m.cols[c] * v.data[c];
        return r;
    }
};

namespace impl
{
/// builds an identity matrix from unit column vectors (rectangular identity when C != R).
template <int C, int R, class T>
[[nodiscard]] constexpr mat<C, R, T> make_identity()
{
    mat<C, R, T> m;
    int const n = C < R ? C : R;
    for (int j = 0; j < n; ++j)
        m.cols[j] = vec<R, T>::make_unit(j);
    return m;
}
} // namespace impl

template <int C, int R, class T>
inline mat<C, R, T> const mat<C, R, T>::zero = mat<C, R, T>{};
template <int C, int R, class T>
inline mat<C, R, T> const mat<C, R, T>::identity = tg::impl::make_identity<C, R, T>();

/// matrix * matrix: lhs is R x C, rhs is C x K, result is R x K (result column j = lhs * rhs column j).
template <int C, int R, int K, class T>
[[nodiscard]] constexpr mat<K, R, T> operator*(mat<C, R, T> const& a, mat<K, C, T> const& b)
{
    mat<K, R, T> m;
    for (int j = 0; j < K; ++j)
        m.cols[j] = a * b.cols[j];
    return m;
}

} // namespace tg
