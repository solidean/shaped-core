#pragma once

#include <typed-geometry/scalar/traits.hh>

/// Curated scalar include: scalar traits plus the free scalar operations that dispatch through them.

namespace tg
{
/// square root of a scalar, dispatched through scalar_traits<T>.
/// only available for scalar types whose trait declares has_sqrt.
template <class T>
[[nodiscard]] T sqrt(T x)
    requires(tg::traits::has_sqrt<T>)
{
    return scalar_traits<T>::sqrt(x);
}
} // namespace tg
