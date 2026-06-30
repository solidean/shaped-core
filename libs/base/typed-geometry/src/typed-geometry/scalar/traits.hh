#pragma once

#include <typed-geometry/fwd.hh>

#include <cmath>

/// Scalar trait seam for typed-geometry.
///
/// tg avoids the std type-traits / <cmath> directly because they are not extensible enough for
/// the scalar types we want to support later (expression trees, double-double, bigint/bigrat, ...).
/// Instead every scalar capability is routed through tg::scalar_traits<T>, a primary template that
/// is specialized per scalar type. tg::traits::has_sqrt<T> and the free tg::sqrt() (see scalar.hh)
/// are thin aliases over the trait entries.
///
/// To teach tg about a new scalar type, specialize tg::scalar_traits for it.
///
///     template <>
///     struct tg::scalar_traits<my_scalar>
///     {
///         static constexpr bool has_sqrt = true;
///         static my_scalar sqrt(my_scalar x) { return my_sqrt(x); }
///     };

namespace tg
{
/// Primary template — undefined capabilities by default.
/// Specialize for each scalar type to opt into operations.
template <class T>
struct scalar_traits
{
    static constexpr bool has_sqrt = false;
};

// NOTE: std::sqrt honors errno, which is a historic mistake and produces worse codegen.
// We route through it for now but intend to replace it — see libs/base/typed-geometry/docs/TODO.md.
template <>
struct scalar_traits<f32>
{
    static constexpr bool has_sqrt = true;

    [[nodiscard]] static f32 sqrt(f32 x) { return std::sqrt(x); }
};

template <>
struct scalar_traits<f64>
{
    static constexpr bool has_sqrt = true;

    [[nodiscard]] static f64 sqrt(f64 x) { return std::sqrt(x); }
};

template <>
struct scalar_traits<i32>
{
    static constexpr bool has_sqrt = false;
};

namespace traits
{
/// true if scalar_traits<T> provides a sqrt() operation.
template <class T>
inline constexpr bool has_sqrt = scalar_traits<T>::has_sqrt;
} // namespace traits

} // namespace tg
