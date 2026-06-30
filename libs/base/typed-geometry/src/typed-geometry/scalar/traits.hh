#pragma once

#include <typed-geometry/scalar/fwd.hh>

#include <cmath>
#include <type_traits>

/// Scalar trait seam for typed-geometry.
///
/// tg avoids the std type-traits / <cmath> directly because they are not extensible enough for
/// the scalar types we want to support later (expression trees, double-double, bigint/bigrat, ...).
/// Instead every scalar capability is routed through tg::scalar_traits<T>, a primary template that
/// is specialized per scalar type. The tg::traits::* helpers and the free functions in scalar.hh
/// (tg::one, tg::sqrt, tg::sin, tg::cos, tg::sin_cos, tg::atan2) are thin wrappers over the entries.
///
/// IMPORTANT: the trait kernels here are the *raw numeric* layer — sin/cos/atan2 work in bare radian
/// T values (sin/cos: T radians -> T, atan2: T,T -> T radians). The angle typing lives one layer up
/// in scalar.hh, where the public tg::sin/tg::cos take a tg::angle and tg::atan2 returns one. Keep
/// scalar specializations free of tg::angle so a new scalar only has to provide plain math.
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

namespace tg
{
/// Primary template — no capabilities by default. Specialize for each scalar type to opt in.
/// Note there is deliberately no default one()/sqrt()/...: a scalar must declare what it supports.
template <class T>
struct scalar_traits
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
};

// NOTE: std::sqrt and the std trig functions honor errno, which is a historic mistake and produces
// worse codegen. We route through them for now but intend to replace them — see
// libs/base/typed-geometry/docs/TODO.md.
template <>
struct scalar_traits<f32>
{
    static constexpr bool has_sqrt = true;
    static constexpr bool has_trigonometry = true;

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
};

template <>
struct scalar_traits<f64>
{
    static constexpr bool has_sqrt = true;
    static constexpr bool has_trigonometry = true;

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
};

// All integer types are scalars. `signed char` / `unsigned char` count as integers here, but plain
// `char` deliberately does not (it falls through to the primary, with no scalar capabilities), and
// `bool` has its own specialization below.
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
struct scalar_traits<T>
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;

    [[nodiscard]] static constexpr T one() { return T(1); }
    [[nodiscard]] static constexpr bool is_zero(T x) { return x == T(0); }
    [[nodiscard]] static constexpr bool is_one(T x) { return x == T(1); }
};

template <>
struct scalar_traits<bool>
{
    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;

    [[nodiscard]] static constexpr bool one() { return true; }
    [[nodiscard]] static constexpr bool is_zero(bool x) { return !x; }
    [[nodiscard]] static constexpr bool is_one(bool x) { return x; }
};

namespace traits
{
/// true if scalar_traits<T> provides a sqrt() operation.
template <class T>
inline constexpr bool has_sqrt = scalar_traits<T>::has_sqrt;

/// true if scalar_traits<T> provides sin()/cos()/atan2() operations.
template <class T>
inline constexpr bool has_trigonometry = scalar_traits<T>::has_trigonometry;

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
