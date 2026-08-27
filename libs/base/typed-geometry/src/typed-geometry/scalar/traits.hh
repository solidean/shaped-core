#pragma once

#include <clean-core/common/assert.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/scalar/fwd.hh>

#include <cmath>
#include <type_traits>

/// Scalar trait seam for typed-geometry.
///
/// Every scalar capability is routed through tg::scalar_traits<T>, a primary template specialized per scalar type.
/// The tg::traits::* helpers and the free functions in scalar.hh are thin wrappers over its entries.
/// That is tg::one, tg::sqrt, tg::abs, the trigonometry, tg::pow / tg::exp / tg::log, tg::round / tg::floor / tg::ceil,
/// and the exact base-two family tg::pow2 / tg::scale_by_pow2 / tg::exponent_of / tg::split_pow2.
/// libs/base/typed-geometry/docs/modules/scalar.md has the why.
///
/// The kernels here are the *raw numeric* layer: sin/cos take a bare radian T and return T, atan2 takes two T and returns radians.
/// The angle typing lives one layer up in scalar.hh, where the public tg::sin/tg::cos take a tg::angle and tg::atan2 returns one.
/// A specialization must stay free of tg::angle, so a new scalar only ever has to provide plain math.
///
/// To teach tg about a new scalar type, specialize tg::scalar_traits for it.
///
///     template <>
///     struct tg::scalar_traits<my_scalar>
///     {
///         static constexpr bool has_sqrt = true;
///         static constexpr my_scalar one() { return my_one(); }
///         static constexpr bool is_zero(my_scalar x) { return is_my_zero(x); }
///         static constexpr bool is_one(my_scalar x) { return is_my_one(x); }
///         static my_scalar sqrt(my_scalar x) { return my_sqrt(x); }
///     };

/// A scalar decomposed against base two: `x == significand * 2^exponent`, exactly.
///
/// The significand is in [1, 2) and carries x's sign, so `exponent` is `floor(log2(|x|))` and reassembling the pair
/// returns x's original bit pattern.
/// That range is deliberately NOT C's `frexp` convention of [0.5, 1) — this one is what the IEEE-754 exponent field
/// already means, so `split_pow2(8.0f)` reads as `{1, 3}` rather than `{0.5, 4}`.
/// A caller porting frexp-shaped code adjusts the exponent by one, and the different name is what makes that visible.
template <class T>
struct tg::pow2_split
{
    T significand = {};
    int exponent = 0;
};

/// Primary template — no capabilities by default.
/// Specialize it per scalar type to opt in; there is deliberately no default one()/sqrt()/..., so a scalar must declare what it supports.
template <class T>
struct tg::scalar_traits
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = false;
    static constexpr bool has_pow2 = false;
};

// std::sqrt and the std trig functions honor errno, which costs codegen for a contract nobody wants.
// Routed through them for now; libs/base/typed-geometry/docs/TODO.md tracks the replacement.
template <>
struct tg::scalar_traits<cc::f32>
{
    static constexpr bool has_sqrt = true;
    static constexpr bool has_trigonometry = true;
    static constexpr bool has_exponential = true;
    static constexpr bool has_rounding = true;
    static constexpr bool has_abs = true;
    static constexpr bool has_pow2 = true;

    [[nodiscard]] static constexpr f32 one() { return 1.0f; }
    [[nodiscard]] static constexpr bool is_zero(f32 x) { return x == 0.0f; }
    [[nodiscard]] static constexpr bool is_one(f32 x) { return x == 1.0f; }
    [[nodiscard]] static f32 sqrt(f32 x) { return std::sqrt(x); }
    [[nodiscard]] static f32 sin(f32 x) { return std::sin(x); }
    [[nodiscard]] static f32 cos(f32 x) { return std::cos(x); }
    [[nodiscard]] static f32 tan(f32 x) { return std::tan(x); }
    [[nodiscard]] static f32 asin(f32 x) { return std::asin(x); }
    [[nodiscard]] static f32 acos(f32 x) { return std::acos(x); }
    [[nodiscard]] static f32 atan(f32 x) { return std::atan(x); }
    [[nodiscard]] static f32 atan2(f32 y, f32 x) { return std::atan2(y, x); }
    [[nodiscard]] static f32 pow(f32 base, f32 exponent) { return std::pow(base, exponent); }
    [[nodiscard]] static f32 exp(f32 x) { return std::exp(x); }
    [[nodiscard]] static f32 log(f32 x) { return std::log(x); }
    [[nodiscard]] static f32 round(f32 x) { return std::round(x); }
    [[nodiscard]] static f32 floor(f32 x) { return std::floor(x); }
    [[nodiscard]] static f32 ceil(f32 x) { return std::ceil(x); }
    [[nodiscard]] static f32 abs(f32 x) { return std::fabs(x); }

    /// x * 2^n, saturating to +-infinity or zero when the result leaves the representable range.
    [[nodiscard]] static f32 scale_by_pow2(f32 x, int n) { return std::ldexp(x, n); }

    /// x must be finite and non-zero: a subnormal is normalized, but zero and the non-finites have no exponent.
    [[nodiscard]] static pow2_split<f32> split_pow2(f32 x)
    {
        CC_ASSERT(std::isfinite(x) && x != f32(0), "split_pow2 needs a finite, non-zero scalar");

        auto exponent = 0;
        auto const half = std::frexp(x, &exponent); // in [0.5, 1) — renormalized to [1, 2) below
        return {.significand = half * f32(2), .exponent = exponent - 1};
    }
};

template <>
struct tg::scalar_traits<cc::f64>
{
    static constexpr bool has_sqrt = true;
    static constexpr bool has_trigonometry = true;
    static constexpr bool has_exponential = true;
    static constexpr bool has_rounding = true;
    static constexpr bool has_abs = true;
    static constexpr bool has_pow2 = true;

    [[nodiscard]] static constexpr f64 one() { return 1.0; }
    [[nodiscard]] static constexpr bool is_zero(f64 x) { return x == 0.0; }
    [[nodiscard]] static constexpr bool is_one(f64 x) { return x == 1.0; }
    [[nodiscard]] static f64 sqrt(f64 x) { return std::sqrt(x); }
    [[nodiscard]] static f64 sin(f64 x) { return std::sin(x); }
    [[nodiscard]] static f64 cos(f64 x) { return std::cos(x); }
    [[nodiscard]] static f64 tan(f64 x) { return std::tan(x); }
    [[nodiscard]] static f64 asin(f64 x) { return std::asin(x); }
    [[nodiscard]] static f64 acos(f64 x) { return std::acos(x); }
    [[nodiscard]] static f64 atan(f64 x) { return std::atan(x); }
    [[nodiscard]] static f64 atan2(f64 y, f64 x) { return std::atan2(y, x); }
    [[nodiscard]] static f64 pow(f64 base, f64 exponent) { return std::pow(base, exponent); }
    [[nodiscard]] static f64 exp(f64 x) { return std::exp(x); }
    [[nodiscard]] static f64 log(f64 x) { return std::log(x); }
    [[nodiscard]] static f64 round(f64 x) { return std::round(x); }
    [[nodiscard]] static f64 floor(f64 x) { return std::floor(x); }
    [[nodiscard]] static f64 ceil(f64 x) { return std::ceil(x); }
    [[nodiscard]] static f64 abs(f64 x) { return std::fabs(x); }

    /// x * 2^n, saturating to +-infinity or zero when the result leaves the representable range.
    [[nodiscard]] static f64 scale_by_pow2(f64 x, int n) { return std::ldexp(x, n); }

    /// x must be finite and non-zero: a subnormal is normalized, but zero and the non-finites have no exponent.
    [[nodiscard]] static pow2_split<f64> split_pow2(f64 x)
    {
        CC_ASSERT(std::isfinite(x) && x != f64(0), "split_pow2 needs a finite, non-zero scalar");

        auto exponent = 0;
        auto const half = std::frexp(x, &exponent); // in [0.5, 1) — renormalized to [1, 2) below
        return {.significand = half * f64(2), .exponent = exponent - 1};
    }
};

// All integer types are scalars, `signed char` and `unsigned char` included.
// Plain `char` deliberately is not: it falls through to the primary and has no scalar capabilities.
// `bool` has its own specialization below.
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
struct tg::scalar_traits<T>
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = true;
    static constexpr bool has_pow2 = false;

    [[nodiscard]] static constexpr T one() { return T(1); }
    [[nodiscard]] static constexpr bool is_zero(T x) { return x == T(0); }
    [[nodiscard]] static constexpr bool is_one(T x) { return x == T(1); }

    /// x must not be the type's most negative value, which has no representable magnitude.
    [[nodiscard]] static constexpr T abs(T x)
    {
        if constexpr (std::is_signed_v<T>)
            return x < T(0) ? T(-x) : x;
        else
            return x;
    }
};

template <>
struct tg::scalar_traits<bool>
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = false;
    static constexpr bool has_pow2 = false;

    [[nodiscard]] static constexpr bool one() { return true; }
    [[nodiscard]] static constexpr bool is_zero(bool x) { return !x; }
    [[nodiscard]] static constexpr bool is_one(bool x) { return x; }
};

namespace tg
{

namespace traits
{
/// true if scalar_traits<T> provides a sqrt() operation.
template <class T>
inline constexpr bool has_sqrt = scalar_traits<T>::has_sqrt;

/// true if scalar_traits<T> provides sin()/cos()/atan2() operations.
template <class T>
inline constexpr bool has_trigonometry = scalar_traits<T>::has_trigonometry;

/// true if scalar_traits<T> provides pow()/exp()/log() operations.
template <class T>
inline constexpr bool has_exponential = scalar_traits<T>::has_exponential;

/// true if scalar_traits<T> provides round()/floor()/ceil() operations.
/// Integers deliberately do not: rounding one is the identity, and asking for it is a sign the caller meant a float.
template <class T>
inline constexpr bool has_rounding = scalar_traits<T>::has_rounding;

/// true if scalar_traits<T> provides an abs() operation.
template <class T>
inline constexpr bool has_abs = scalar_traits<T>::has_abs;

/// true if scalar_traits<T> provides the exact base-two operations scale_by_pow2() and split_pow2().
/// Integers deliberately do not: shifting one truncates, which is a different operation wearing the same name.
template <class T>
inline constexpr bool has_pow2 = scalar_traits<T>::has_pow2;

/// is the value the additive identity? Routed through scalar_traits so symbolic / bigint / ...
/// scalars can supply a smarter test than a plain comparison.
template <class T>
[[nodiscard]] constexpr bool is_zero(T const& x)
{
    return scalar_traits<T>::is_zero(x);
}

/// is the value the multiplicative identity? Routed through scalar_traits (see is_zero).
template <class T>
[[nodiscard]] constexpr bool is_one(T const& x)
{
    return scalar_traits<T>::is_one(x);
}
} // namespace traits

} // namespace tg
