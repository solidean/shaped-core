#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/traits.hh>

namespace tg
{
/// A scalar angle — a domain newtype over T whose storage is always radians.
///
/// angle exists to stop radian/degree mix-ups: you cannot construct one from a bare scalar, only
/// via the explicit make_from_radians / make_from_degree factories, and you read it back with the
/// explicit .radians() / .degree() accessors. It supports addition and scalar multiplication (it is
/// a 1D vector space over T) but performs no wrap-around — it is a unit-checked number, not a
/// modular [0, 2pi) value.
///
///     auto const a = tg::angle_f::make_from_degree(90);
///     auto const r = a.radians();               // ~1.5708
///     auto const b = a + a;                      // 180 degrees
///     using namespace tg::literals;
///     auto const c = 45_deg_f + 0.5_rad_f;
template <class T>
struct angle
{
    // construction
public:
    angle() = default;

    [[nodiscard]] static constexpr angle make_from_radians(T radians)
    {
        angle a;
        a._radians = radians;
        return a;
    }

    [[nodiscard]] static constexpr angle make_from_degree(T degree)
    {
        angle a;
        a._radians = degree * (tg::pi<T> / T(180));
        return a;
    }

    // access
public:
    [[nodiscard]] constexpr T radians() const { return _radians; }
    [[nodiscard]] constexpr T degree() const { return _radians * (T(180) / tg::pi<T>); }

    // trigonometry (only for scalars with has_trigonometry)
    //
    // These mirror the free tg::sin/cos/... in scalar.hh; both forms exist so you can write either
    // a.sin() or tg::sin(a). sec/csc/cot are the reciprocals of cos/sin/tan.
public:
    [[nodiscard]] T sin() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::sin(_radians);
    }
    [[nodiscard]] T cos() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::cos(_radians);
    }
    [[nodiscard]] T tan() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::tan(_radians);
    }

    /// both sine and cosine as a pair {sin, cos}.
    [[nodiscard]] cc::pair<T, T> sin_cos() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return {scalar_traits<T>::sin(_radians), scalar_traits<T>::cos(_radians)};
    }

    [[nodiscard]] T sec() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::one() / scalar_traits<T>::cos(_radians);
    }
    [[nodiscard]] T csc() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::one() / scalar_traits<T>::sin(_radians);
    }
    [[nodiscard]] T cot() const
        requires(tg::traits::has_trigonometry<T>)
    {
        return scalar_traits<T>::cos(_radians) / scalar_traits<T>::sin(_radians);
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(angle const&, angle const&) = default;

    // arithmetic (1D vector space; no wrap-around)
public:
    [[nodiscard]] friend constexpr angle operator+(angle a, angle b)
    {
        return make_from_radians(a._radians + b._radians);
    }
    [[nodiscard]] friend constexpr angle operator-(angle a, angle b)
    {
        return make_from_radians(a._radians - b._radians);
    }
    [[nodiscard]] friend constexpr angle operator-(angle a) { return make_from_radians(-a._radians); }
    [[nodiscard]] friend constexpr angle operator*(angle a, T s) { return make_from_radians(a._radians * s); }
    [[nodiscard]] friend constexpr angle operator*(T s, angle a) { return make_from_radians(a._radians * s); }
    [[nodiscard]] friend constexpr angle operator/(angle a, T s) { return make_from_radians(a._radians / s); }

private:
    T _radians = {};
};

/// User-defined literals for angles. Kept in their own namespace; tg re-exports them so tg-internal
/// code can write `90_deg_f` directly. Downstream code can opt in with `using namespace tg::literals;`.
namespace literals
{
[[nodiscard]] constexpr angle_f operator""_rad_f(long double v)
{
    return angle_f::make_from_radians(f32(v));
}
[[nodiscard]] constexpr angle_f operator""_rad_f(unsigned long long v)
{
    return angle_f::make_from_radians(f32(v));
}
[[nodiscard]] constexpr angle_d operator""_rad_d(long double v)
{
    return angle_d::make_from_radians(f64(v));
}
[[nodiscard]] constexpr angle_d operator""_rad_d(unsigned long long v)
{
    return angle_d::make_from_radians(f64(v));
}

[[nodiscard]] constexpr angle_f operator""_deg_f(long double v)
{
    return angle_f::make_from_degree(f32(v));
}
[[nodiscard]] constexpr angle_f operator""_deg_f(unsigned long long v)
{
    return angle_f::make_from_degree(f32(v));
}
[[nodiscard]] constexpr angle_d operator""_deg_d(long double v)
{
    return angle_d::make_from_degree(f64(v));
}
[[nodiscard]] constexpr angle_d operator""_deg_d(unsigned long long v)
{
    return angle_d::make_from_degree(f64(v));
}
} // namespace literals

using namespace literals;

} // namespace tg
