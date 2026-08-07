#pragma once

#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace tg::impl
{
/// The rotation part of a transform, per dimension: a unit complex number in 2D, a unit quaternion in 3D.
///
/// Unlike quat, this default-constructs to the IDENTITY rotation,
/// which is what makes a default homogeneous_transform the identity rather than a singular map.
///
/// This is deliberately not public API.
/// The type that should eventually replace it is a real tg::rotor<D, T> in linalg,
/// which would also give the 2D rotation a public name.
template <int D, class T>
struct rotation_representation;

template <class T>
struct rotation_representation<2, T>
{
    // unit complex number cos + i*sin
    T cos = tg::one<T>();
    T sin = {};

    [[nodiscard]] static rotation_representation make_rotation(angle<T> a)
        requires(tg::traits::has_trigonometry<T>)
    {
        auto const [s, c] = tg::sin_cos(a);
        return {.cos = c, .sin = s};
    }

    [[nodiscard]] angle<T> to_angle() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return tg::atan2(sin, cos);
    }

    [[nodiscard]] constexpr vec<2, T> apply(vec<2, T> const& v) const
    {
        return vec<2, T>(cos * v.data[0] - sin * v.data[1], sin * v.data[0] + cos * v.data[1]);
    }

    /// the rotation that applies `b` first, then this one.
    [[nodiscard]] constexpr rotation_representation compose(rotation_representation const& b) const
    {
        return {.cos = cos * b.cos - sin * b.sin, .sin = sin * b.cos + cos * b.sin};
    }

    [[nodiscard]] constexpr rotation_representation inverse() const { return {.cos = cos, .sin = -sin}; }

    [[nodiscard]] constexpr mat<2, 2, T> to_rotation_matrix() const
    {
        return mat<2, 2, T>::make_from_cols(vec<2, T>(cos, sin), vec<2, T>(-sin, cos));
    }

    [[nodiscard]] friend constexpr bool operator==(rotation_representation const&, rotation_representation const&)
        = default;
};

template <class T>
struct rotation_representation<3, T>
{
    quat<T> rotation = quat<T>::make_identity();

    [[nodiscard]] static constexpr rotation_representation make_rotation(quat<T> const& q) { return {.rotation = q}; }

    [[nodiscard]] constexpr quat<T> to_quat() const { return rotation; }

    [[nodiscard]] constexpr vec<3, T> apply(vec<3, T> const& v) const { return rotation * v; }

    /// the rotation that applies `b` first, then this one.
    [[nodiscard]] constexpr rotation_representation compose(rotation_representation const& b) const
    {
        return {.rotation = rotation * b.rotation};
    }

    [[nodiscard]] constexpr rotation_representation inverse() const { return {.rotation = rotation.conjugate()}; }

    [[nodiscard]] constexpr mat<3, 3, T> to_rotation_matrix() const { return rotation.to_rotation_matrix(); }

    [[nodiscard]] friend constexpr bool operator==(rotation_representation const&, rotation_representation const&)
        = default;
};

} // namespace tg::impl
