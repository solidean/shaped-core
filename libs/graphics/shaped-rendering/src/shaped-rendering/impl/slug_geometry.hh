#pragma once

#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// The 2D helpers Slug's outline code shares.
// tg has no equivalent yet: no 2D wedge product, no perpendicular with a promised turn, no midpoint or lerp of positions.
// libs/graphics/shaped-rendering/docs/lower-library-gaps.md records them, and they leave here when tg gains them.

namespace sr::impl
{
/// The 2D wedge product: positive when `b` turns left of `a`, toward +y from +x.
[[nodiscard]] inline f32 cross(tg::vec2f a, tg::vec2f b)
{
    return a[0] * b[1] - a[1] * b[0];
}

/// `v` turned a quarter from +x toward +y.
[[nodiscard]] inline tg::vec2f left_of(tg::vec2f v)
{
    return tg::vec2f(-v[1], v[0]);
}

[[nodiscard]] inline tg::pos2f midpoint(tg::pos2f a, tg::pos2f b)
{
    return tg::pos2f((a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f);
}

[[nodiscard]] inline tg::pos2f lerp(tg::pos2f a, tg::pos2f b, f32 t)
{
    return tg::pos2f(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
}
} // namespace sr::impl
