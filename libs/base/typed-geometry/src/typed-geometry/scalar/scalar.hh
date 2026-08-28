#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/traits.hh>

#include <type_traits>

/// Curated scalar include: the traits and constants, plus the free scalar operations that dispatch through them.
/// Prefer these free functions over std:: math so exotic scalar types keep working.
///
/// Trigonometry is expressed in terms of tg::angle rather than bare radians, so the unit is checked at the type level.
/// sin/cos take an angle and return a scalar; atan2 takes two scalars and returns an angle.
/// The underlying scalar_traits kernels still work in raw radian T, so the angle typing lives only at this public layer.

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

/// magnitude, dispatched through scalar_traits<T>; only for scalars with has_abs.
/// For an integer, x must not be the type's most negative value, which has no representable magnitude.
template <class T>
[[nodiscard]] constexpr T abs(T x)
    requires(tg::traits::has_abs<T>)
{
    return scalar_traits<T>::abs(x);
}

// Exponentials — base and exponent share one type, so a mixed-type call needs an explicit cast rather than a silent promotion.
// All require a scalar with has_exponential.

/// base raised to exponent.
template <class T>
[[nodiscard]] T pow(T base, T exponent)
    requires(tg::traits::has_exponential<T>)
{
    return scalar_traits<T>::pow(base, exponent);
}

/// e raised to x.
template <class T>
[[nodiscard]] T exp(T x)
    requires(tg::traits::has_exponential<T>)
{
    return scalar_traits<T>::exp(x);
}

/// natural logarithm; x must be positive.
template <class T>
[[nodiscard]] T log(T x)
    requires(tg::traits::has_exponential<T>)
{
    return scalar_traits<T>::log(x);
}

// Base two, exactly — an exponent adjustment rather than a multiply, and lossless where the result is representable.
// This is what a shared-exponent pixel format, a half-float conversion or a fixed-point normalization is made of,
// so it is deliberately separate from the approximate tg::pow / tg::exp above.
// All require a scalar with has_pow2.

/// 2^n, exactly.
/// n outside the scalar's exponent range saturates to infinity or zero, as scale_by_pow2 does.
/// The name says `by_int` because the exponent is an exact integer: `pow2` alone would read as an approximate
/// `T -> T`, which is a different function and the one that would sit beside tg::exp and tg::log.
template <class T, class N>
[[nodiscard]] T pow2_by_int(N n)
    requires(tg::traits::has_pow2<T> && std::is_integral_v<N> && !std::is_same_v<N, bool>)
{
    return scalar_traits<T>::scale_by_pow2(tg::one<T>(), int(n));
}

/// x * 2^n, exact for every result the scalar can represent, saturating to +-infinity or zero beyond that.
/// n must be an integer type: a float exponent would truncate silently, so it is a compile error instead.
template <class T, class N>
[[nodiscard]] T scale_by_pow2(T x, N n)
    requires(tg::traits::has_pow2<T> && std::is_integral_v<N> && !std::is_same_v<N, bool>)
{
    return scalar_traits<T>::scale_by_pow2(x, int(n));
}

/// The exponent of x against base two — floor(log2(|x|)), exactly and without a logarithm.
/// x must be finite and non-zero.
template <class T>
[[nodiscard]] int exponent_of(T x)
    requires(tg::traits::has_pow2<T>)
{
    return scalar_traits<T>::split_pow2(x).exponent;
}

/// Both halves at once: x == significand * 2^exponent with the significand in [1, 2), NOT frexp's [0.5, 1).
/// x must be finite and non-zero.
template <class T>
[[nodiscard]] pow2_split<T> split_pow2(T x)
    requires(tg::traits::has_pow2<T>)
{
    return scalar_traits<T>::split_pow2(x);
}

// Rounding — each returns the same scalar type rather than an integer, so the caller decides what to narrow to.
// All require a scalar with has_rounding.

/// nearest integral value, halfway cases away from zero.
template <class T>
[[nodiscard]] T round(T x)
    requires(tg::traits::has_rounding<T>)
{
    return scalar_traits<T>::round(x);
}

/// largest integral value not greater than x.
template <class T>
[[nodiscard]] T floor(T x)
    requires(tg::traits::has_rounding<T>)
{
    return scalar_traits<T>::floor(x);
}

/// smallest integral value not less than x.
template <class T>
[[nodiscard]] T ceil(T x)
    requires(tg::traits::has_rounding<T>)
{
    return scalar_traits<T>::ceil(x);
}

// Trigonometry — the free-function forms of the angle members (a.sin() and friends).
// Both forms exist, and these delegate, so there is a single implementation.
// All require a scalar with has_trigonometry.

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

// Inverse trigonometry — these take a scalar and *return* an angle, like atan2.
// All require a scalar with has_trigonometry.

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

/// two-argument arctangent; returns the angle of the vector (x, y).
/// Only for scalars with has_trigonometry.
template <class T>
[[nodiscard]] angle<T> atan2(T y, T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return angle<T>::make_from_radians(scalar_traits<T>::atan2(y, x));
}
} // namespace tg
