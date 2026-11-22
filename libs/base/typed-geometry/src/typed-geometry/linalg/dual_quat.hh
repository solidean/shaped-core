#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Dual quaternion `real + ε dual`: a rigid motion in a form that blends.
///
/// A unit one is a rotation `real` followed by a translation t, with `dual = (t, 0) * real / 2`.
/// That is what tg::rigid_transform3 holds too; a dual quaternion is the form to blend or interpolate rigid motions in,
/// where a weighted sum of rotations and translations would shear and shrink (tg::blend, tg::sclerp).
/// It is a number with an algebra, like tg::quat, not a transform class: `a * b` composes, `d * p` applies.
/// Default construction yields the zero dual quaternion, so use dual_quat::identity for the motion that does nothing.
///
///     auto const d = tg::dual_quat_f::make_rigid(tg::quat_f::make_rotation_z(90_deg_f), tg::vec3f(1, 0, 0));
///     auto const p = d * tg::pos3f(1, 0, 0);   // ~ (1, 1, 0): rotated, then moved
template <class T>
struct tg::dual_quat
{
    using scalar_t = T;

    quat<T> real;
    quat<T> dual;

    // construction
public:
    dual_quat() = default;

    explicit constexpr dual_quat(quat<T> const& real, quat<T> const& dual) : real(real), dual(dual) {}

    // special values
public:
    /// the zero dual quaternion.
    /// Runtime constant, not usable in constant expressions.
    static dual_quat const zero;
    /// the identity motion: real (0, 0, 0, 1), dual zero.
    /// Runtime constant, not usable in constant expressions.
    static dual_quat const identity;

    /// the identity motion, as a constant expression.
    [[nodiscard]] static constexpr dual_quat make_identity() { return dual_quat(quat<T>::make_identity(), quat<T>()); }

    // rigid motions
public:
    /// rotation by the unit quaternion `rotation`, about the origin.
    [[nodiscard]] static constexpr dual_quat make_rotation(quat<T> const& rotation)
    {
        return dual_quat(rotation, quat<T>());
    }

    [[nodiscard]] static constexpr dual_quat make_translation(vec<3, T> const& translation)
    {
        auto const h = translation * T(0.5);
        return dual_quat(quat<T>::make_identity(), quat<T>(h.data[0], h.data[1], h.data[2], T{}));
    }

    /// rotation by the unit quaternion `rotation` about the origin, then translation: what a rigid_transform3 is.
    [[nodiscard]] static constexpr dual_quat make_rigid(quat<T> const& rotation, vec<3, T> const& translation)
    {
        return make_translation(translation) * make_rotation(rotation);
    }

    // the rigid motion a unit dual quaternion is
public:
    /// the rotation, applied first; unit when this is.
    [[nodiscard]] constexpr quat<T> rotation() const { return real; }

    /// the translation, applied after the rotation.
    /// This must be unit: for any other, the result is scaled by |real|².
    [[nodiscard]] constexpr vec<3, T> translation() const
    {
        auto const t = dual * real.conjugate();
        return vec<3, T>(T(2) * t.data[0], T(2) * t.data[1], T(2) * t.data[2]);
    }

    // conjugates: a dual quaternion has three, and none is the obvious one
public:
    /// both parts conjugated as quaternions: the inverse motion of a unit dual quaternion.
    [[nodiscard]] constexpr dual_quat quat_conjugate() const { return dual_quat(real.conjugate(), dual.conjugate()); }

    /// the dual part negated, ε -> -ε, as for a dual number.
    [[nodiscard]] constexpr dual_quat dual_conjugate() const { return dual_quat(real, -dual); }

    /// both conjugations together: what the sandwich `d (1 + ε p) full_conjugate(d)` applies a motion to a point with.
    [[nodiscard]] constexpr dual_quat full_conjugate() const { return dual_quat(real.conjugate(), -dual.conjugate()); }

    /// the inverse motion.
    /// This must be unit; it is quat_conjugate(), named for what it means.
    [[nodiscard]] constexpr dual_quat inverse() const { return quat_conjugate(); }

    // measures
public:
    /// the nearest unit dual quaternion: both parts divided by |real|, and the dual part's component along the real part removed.
    /// Dividing alone, which suffices for applying the motion, leaves a dual quaternion that is not a rigid motion,
    /// so its translation() and inverse() would be wrong.
    /// A zero real part yields zero, as quat::normalized() does.
    [[nodiscard]] dual_quat normalized() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const l = real.length();
        if (tg::traits::is_zero(l))
            return zero;
        auto const r = real * (T(1) / l);
        auto const d = dual * (T(1) / l);
        return dual_quat(r, d - r * tg::dot(r, d));
    }

    // comparison
public:
    /// representational, not geometric: d and -d are one motion but do not compare equal, as for tg::quat.
    [[nodiscard]] friend constexpr bool operator==(dual_quat const&, dual_quat const&) = default;

    // arithmetic
public:
    /// composition of motions — (a * b) applies b first, then a.
    [[nodiscard]] friend constexpr dual_quat operator*(dual_quat const& a, dual_quat const& b)
    {
        return dual_quat(a.real * b.real, a.real * b.dual + a.dual * b.real);
    }

    /// the sum, part by part: what a blend is made of; not a rigid motion until normalized().
    [[nodiscard]] friend constexpr dual_quat operator+(dual_quat const& a, dual_quat const& b)
    {
        return dual_quat(a.real + b.real, a.dual + b.dual);
    }
    [[nodiscard]] friend constexpr dual_quat operator-(dual_quat const& a, dual_quat const& b)
    {
        return dual_quat(a.real - b.real, a.dual - b.dual);
    }
    [[nodiscard]] friend constexpr dual_quat operator-(dual_quat const& a) { return dual_quat(-a.real, -a.dual); }
    [[nodiscard]] friend constexpr dual_quat operator*(dual_quat const& a, T s)
    {
        return dual_quat(a.real * s, a.dual * s);
    }
    [[nodiscard]] friend constexpr dual_quat operator*(T s, dual_quat const& a)
    {
        return dual_quat(a.real * s, a.dual * s);
    }

    /// rotates a vector: a direction has no position, so the translation does not apply.
    /// This must be unit.
    [[nodiscard]] friend constexpr vec<3, T> operator*(dual_quat const& d, vec<3, T> const& v) { return d.real * v; }

    /// moves a point: rotated, then translated.
    /// This must be unit.
    [[nodiscard]] friend constexpr pos<3, T> operator*(dual_quat const& d, pos<3, T> const& p)
    {
        auto const moved = d.real * vec<3, T>(p.data[0], p.data[1], p.data[2]) + d.translation();
        return pos<3, T>(moved.data[0], moved.data[1], moved.data[2]);
    }
};

namespace tg
{
template <class T>
inline dual_quat<T> const dual_quat<T>::zero = dual_quat<T>{};
template <class T>
inline dual_quat<T> const dual_quat<T>::identity = dual_quat<T>::make_identity();

/// The weighted blend of unit dual quaternions, normalized: dual-quaternion linear blending, as skinning takes it.
/// Each is first brought to the side of the first one's rotation, since d and -d are one motion and a blend across
/// the two sides passes through zero.
/// `motions` and `weights` must be as long as each other and not empty; the weights must sum to more than zero.
/// Sums of weights other than 1 are fine: the normalization divides them out.
template <class T>
[[nodiscard]] dual_quat<T> blend(cc::span<dual_quat<T> const> motions, cc::span<T const> weights)
    requires(tg::traits::has_sqrt<T>)
{
    CC_ASSERT(motions.size() == weights.size(), "every motion needs a weight");
    CC_ASSERT(!motions.empty(), "a blend needs at least one motion");
    auto sum = dual_quat<T>::zero;
    for (auto i = isize(0); i < motions.size(); ++i)
    {
        auto const side = tg::dot(motions[i].real, motions[0].real) < T{} ? -weights[i] : weights[i];
        sum = sum + motions[i] * side;
    }
    return sum.normalized();
}

/// Screw linear interpolation: the motion `t` of the way from `a` to `b` along the single screw that takes one to the other,
/// turning and advancing at constant rates; `t` = 0 is `a`, `t` = 1 is `b`.
/// Both must be unit; the shorter of the two ways round is taken.
template <class T>
[[nodiscard]] dual_quat<T> sclerp(dual_quat<T> const& a, dual_quat<T> const& b, T t)
    requires(tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
{
    auto step = a.inverse() * b;
    if (step.real.data[3] < T{})
        step = -step;

    auto const v = vec<3, T>(step.real.data[0], step.real.data[1], step.real.data[2]);
    auto const sin_half = v.length();
    auto const translation = step.translation();
    if (tg::traits::is_zero(sin_half))
        return a * dual_quat<T>::make_translation(translation * t); // no turn: the screw is a straight slide

    // the screw: its axis direction l through the point it turns about (as the moment m), its angle, and how far it slides
    auto const l = v * (T(1) / sin_half);
    auto const angle = tg::atan2(sin_half, step.real.data[3]) * T(2);
    auto const slide = tg::dot(translation, l);
    auto const [s0, c0] = tg::sin_cos(angle / T(2));
    auto const m = (tg::dual(tg::cross(translation, l)) + (translation - l * slide) * (c0 / s0)) * T(0.5);

    // the same screw, a fraction t of the way
    auto const [s, c] = tg::sin_cos(angle * t / T(2));
    auto const half_slide = slide * t * T(0.5);
    auto const real = quat<T>(l.data[0] * s, l.data[1] * s, l.data[2] * s, c);
    auto const dual_v = m * s + l * (half_slide * c);
    auto const dual = quat<T>(dual_v.data[0], dual_v.data[1], dual_v.data[2], -half_slide * s);
    return a * dual_quat<T>(real, dual);
}
} // namespace tg
