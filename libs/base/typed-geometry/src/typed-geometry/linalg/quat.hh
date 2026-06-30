#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace tg
{
/// Unit quaternion for representing 3D rotations.
///
/// Storage is the public C array member `data` in the order {x, y, z, w}, where (x, y, z) is the
/// vector part and w is the scalar part. Default construction yields the zero quaternion; use
/// quat::identity for the rotation-neutral (0, 0, 0, 1).
///
///     auto q = tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90));
///     tg::vec3f const v = q * tg::vec3f(1, 0, 0);   // ~ (0, 1, 0)
template <class T>
struct quat
{
    T data[4] = {};

    // construction
public:
    quat() = default;

    explicit constexpr quat(T x, T y, T z, T w) : data{x, y, z, w} {}

    // special values
public:
    /// the zero quaternion. Runtime constant, not usable in constant expressions.
    static quat const zero;
    /// the identity rotation (0, 0, 0, 1). Runtime constant.
    static quat const identity;

    // rotations
public:
    /// rotation by `a` around the given axis (assumed normalized).
    [[nodiscard]] static quat make_rotation_axis_angle(vec<3, T> const& axis, angle<T> a)
        requires(tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a / T(2));
        return quat(axis.data[0] * s, axis.data[1] * s, axis.data[2] * s, c);
    }

    [[nodiscard]] static quat make_rotation_x(angle<T> a)
        requires(tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a / T(2));
        return quat(s, T{}, T{}, c);
    }
    [[nodiscard]] static quat make_rotation_y(angle<T> a)
        requires(tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a / T(2));
        return quat(T{}, s, T{}, c);
    }
    [[nodiscard]] static quat make_rotation_z(angle<T> a)
        requires(tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a / T(2));
        return quat(T{}, T{}, s, c);
    }

    // access
public:
    [[nodiscard]] constexpr T& operator[](int i)
    {
        CC_ASSERT(0 <= i && i < 4, "quaternion index out of range");
        return data[i];
    }
    [[nodiscard]] constexpr T const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < 4, "quaternion index out of range");
        return data[i];
    }

    // measures
public:
    [[nodiscard]] constexpr T length_sqr() const
    {
        return data[0] * data[0] + data[1] * data[1] + data[2] * data[2] + data[3] * data[3];
    }
    [[nodiscard]] T length() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::sqrt(this->length_sqr());
    }
    [[nodiscard]] quat normalized() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const l = this->length();
        if (tg::traits::is_zero(l))
            return zero;
        return quat(data[0] / l, data[1] / l, data[2] / l, data[3] / l);
    }

    /// the rotation axis (unit length); the zero vector when there is no rotation.
    [[nodiscard]] vec<3, T> axis() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const v = vec<3, T>(data[0], data[1], data[2]);
        auto const l = v.length();
        if (tg::traits::is_zero(l))
            return vec<3, T>::zero;
        return v / l;
    }

    /// the rotation angle; zero when there is no rotation. Pairs with make_rotation_axis_angle.
    [[nodiscard]] tg::angle<T> angle() const
        requires(tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
    {
        auto const v = vec<3, T>(data[0], data[1], data[2]);
        return T(2) * tg::atan2(v.length(), data[3]); // atan2 already returns an angle
    }

    /// the conjugate (negated vector part); the inverse rotation for a unit quaternion.
    [[nodiscard]] constexpr quat conjugate() const { return quat(-data[0], -data[1], -data[2], data[3]); }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(quat const&, quat const&) = default;

    // arithmetic
public:
    /// Hamilton product: composition of rotations — (a * b) applies b first, then a.
    [[nodiscard]] friend constexpr quat operator*(quat const& a, quat const& b)
    {
        T const ax = a.data[0];
        T const ay = a.data[1];
        T const az = a.data[2];
        T const aw = a.data[3];
        T const bx = b.data[0];
        T const by = b.data[1];
        T const bz = b.data[2];
        T const bw = b.data[3];
        return quat(aw * bx + ax * bw + ay * bz - az * by, //
                    aw * by - ax * bz + ay * bw + az * bx, //
                    aw * bz + ax * by - ay * bx + az * bw, //
                    aw * bw - ax * bx - ay * by - az * bz);
    }

    /// rotates a vector by this quaternion (assumed unit).
    [[nodiscard]] friend constexpr vec<3, T> operator*(quat const& q, vec<3, T> const& v)
    {
        auto const u = vec<3, T>(q.data[0], q.data[1], q.data[2]);
        T const w = q.data[3];
        auto const t = T(2) * tg::dual(tg::cross(u, v));
        return v + w * t + tg::dual(tg::cross(u, t));
    }
};

template <class T>
inline quat<T> const quat<T>::zero = quat<T>{};
template <class T>
inline quat<T> const quat<T>::identity = quat<T>(T{}, T{}, T{}, tg::one<T>());

} // namespace tg
