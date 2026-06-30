#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/traits.hh>

/// Curated scalar include: scalar traits/constants plus the free scalar operations that dispatch
/// through them. Prefer these free functions over std:: math so exotic scalar types keep working.
///
/// Note the angle typing: trigonometry is expressed in terms of tg::angle, not bare radians, so the
/// unit is checked at the type level. sin/cos take an angle and return a scalar; atan2 takes two
/// scalars and returns an angle. The underlying scalar_traits kernels still work in raw radian T
/// (that is the low-level numeric seam) — the angle typing lives only at this public layer.

namespace tg
{
/// the multiplicative identity of T, via scalar_traits<T>.
/// kept a function (not a literal) because not every scalar is constructible from an int.
template <class T>
[[nodiscard]] constexpr T one()
{
    return scalar_traits<T>::one();
}

/// square root, dispatched through scalar_traits<T>; only for scalars with has_sqrt.
template <class T>
[[nodiscard]] T sqrt(T x)
    requires(tg::traits::has_sqrt<T>)
{
    return scalar_traits<T>::sqrt(x);
}

// Trigonometry — free-function forms of the angle members (a.sin() etc.). Both forms exist; these
// delegate so there is a single implementation. All require a scalar with has_trigonometry.

/// sine of an angle.
template <class T>
[[nodiscard]] T sin(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.sin();
}

/// cosine of an angle.
template <class T>
[[nodiscard]] T cos(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.cos();
}

/// tangent of an angle.
template <class T>
[[nodiscard]] T tan(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.tan();
}

/// combined sine and cosine of an angle as a pair {sin(a), cos(a)}.
///
/// TODO: with libm we can use the combined sincos() entry point, which is cheaper than two calls.
template <class T>
[[nodiscard]] cc::pair<T, T> sin_cos(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.sin_cos();
}

/// secant (1 / cos).
template <class T>
[[nodiscard]] T sec(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.sec();
}

/// cosecant (1 / sin).
template <class T>
[[nodiscard]] T csc(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.csc();
}

/// cotangent (cos / sin).
template <class T>
[[nodiscard]] T cot(angle<T> a)
    requires(tg::traits::has_trigonometry<T>)
{
    return a.cot();
}

// Inverse trigonometry — these take a scalar and *return* an angle (like atan2). All require a
// scalar with has_trigonometry.

/// arcsine: the angle whose sine is x (x in [-1, 1]).
template <class T>
[[nodiscard]] angle<T> asin(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return angle<T>::make_from_radians(scalar_traits<T>::asin(x));
}

/// arccosine: the angle whose cosine is x (x in [-1, 1]).
template <class T>
[[nodiscard]] angle<T> acos(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return angle<T>::make_from_radians(scalar_traits<T>::acos(x));
}

/// arctangent: the angle whose tangent is x.
template <class T>
[[nodiscard]] angle<T> atan(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return angle<T>::make_from_radians(scalar_traits<T>::atan(x));
}

/// two-argument arctangent; returns the angle of the vector (x, y). Only for scalars with
/// has_trigonometry.
template <class T>
[[nodiscard]] angle<T> atan2(T y, T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return angle<T>::make_from_radians(scalar_traits<T>::atan2(y, x));
}
} // namespace tg
