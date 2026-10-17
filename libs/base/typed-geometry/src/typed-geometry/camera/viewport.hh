#pragma once

#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// Pixels and normalized device coordinates.
///
/// Pixel space has its origin at the top-left corner, +y down, and spans [0, size] in each axis.
/// A pixel's center is at its index plus one half, so add 0.5 to an index before converting it.
/// Normalized device coordinates span [-1, 1] with +y up, which is why the two disagree on y's direction.

namespace tg
{
template <class T>
[[nodiscard]] constexpr pos<2, T> ndc_from_pixel(pos<2, T> const& pixel, vec<2, T> const& viewport_size)
{
    return pos<2, T>(T(2) * pixel.data[0] / viewport_size.data[0] - T(1),
                     T(1) - T(2) * pixel.data[1] / viewport_size.data[1]);
}

template <class T>
[[nodiscard]] constexpr pos<2, T> pixel_from_ndc(pos<2, T> const& ndc, vec<2, T> const& viewport_size)
{
    return pos<2, T>((ndc.data[0] + T(1)) / T(2) * viewport_size.data[0],
                     (T(1) - ndc.data[1]) / T(2) * viewport_size.data[1]);
}
} // namespace tg
