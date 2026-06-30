#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/traits.hh>

/// Curated scalar include: scalar traits/constants plus the free scalar operations that dispatch
/// through them. Prefer these free functions over std:: math so exotic scalar types keep working.

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

/// sine, dispatched through scalar_traits<T>; only for scalars with has_trigonometry.
template <class T>
[[nodiscard]] T sin(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return scalar_traits<T>::sin(x);
}

/// cosine, dispatched through scalar_traits<T>; only for scalars with has_trigonometry.
template <class T>
[[nodiscard]] T cos(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return scalar_traits<T>::cos(x);
}

/// combined sine and cosine as a pair {sin(x), cos(x)}.
/// only for scalars with has_trigonometry.
///
/// TODO: with libm we can use the combined sincos() entry point, which is cheaper than two calls.
template <class T>
[[nodiscard]] cc::pair<T, T> sin_cos(T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return {scalar_traits<T>::sin(x), scalar_traits<T>::cos(x)};
}

/// two-argument arctangent, dispatched through scalar_traits<T>; only for scalars with
/// has_trigonometry.
template <class T>
[[nodiscard]] T atan2(T y, T x)
    requires(tg::traits::has_trigonometry<T>)
{
    return scalar_traits<T>::atan2(y, x);
}
} // namespace tg
