#pragma once

#include <typed-geometry/scalar/fwd.hh>

namespace tg
{
/// A uniform grid of cells, each a pos<D, i32>, over D-dimensional space.
template <int D, class T>
struct uniform_grid;

/// One cell a ray passes through, and the stretch of the ray inside it.
template <int D, class T>
struct grid_step;

/// The cells a ray passes through, front to back.
template <int D, class T>
class grid_traversal;

using uniform_grid2f = uniform_grid<2, f32>;
using uniform_grid3f = uniform_grid<3, f32>;
using uniform_grid2d = uniform_grid<2, f64>;
using uniform_grid3d = uniform_grid<3, f64>;
} // namespace tg
