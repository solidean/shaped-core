#pragma once

#include <typed-geometry/scalar/fwd.hh>

/// Mathematical constants, templated on the scalar type.
///
/// The default value is a high-precision literal narrowed to T, which covers the built-in floats.
/// Exotic scalar types that are not constructible from a floating-point literal can specialize the
/// variable template for themselves.

namespace tg
{
template <class T>
inline constexpr T pi = T(3.14159265358979323846264338327950288L);
} // namespace tg
