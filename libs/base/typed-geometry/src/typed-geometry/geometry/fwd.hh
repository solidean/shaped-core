#pragma once

#include <typed-geometry/linalg/fwd.hh>

namespace tg
{
//
// Geometric object types
//
// Each denotes a set of points; see geometry/traits.hh for the object_traits seam
// (intrinsic_dim / ambient_dim / is_finite).

/// axis-aligned bounding box (solid box between min and max). Finite, full-dimensional.
template <int D, class T>
struct aabb;

/// filled triangle (convex hull of three vertices). Finite, intrinsic_dim 2.
template <int D, class T>
struct triangle;

/// line segment between two endpoints (inclusive). Finite, intrinsic_dim 1.
template <int D, class T>
struct segment;

/// half-line {origin + t*dir : t >= 0}. Infinite, intrinsic_dim 1.
template <int D, class T>
struct ray;

/// infinite line {origin + t*dir : t in R}. Infinite, intrinsic_dim 1.
template <int D, class T>
struct line;

/// hyperplane {x : dot(normal, x) == dist}. Infinite, intrinsic_dim D-1.
template <int D, class T>
struct plane;

//
// Dimensional aliases
//

template <class T>
using aabb2 = aabb<2, T>;
template <class T>
using aabb3 = aabb<3, T>;

template <class T>
using triangle2 = triangle<2, T>;
template <class T>
using triangle3 = triangle<3, T>;

template <class T>
using segment2 = segment<2, T>;
template <class T>
using segment3 = segment<3, T>;

template <class T>
using ray2 = ray<2, T>;
template <class T>
using ray3 = ray<3, T>;

template <class T>
using line2 = line<2, T>;
template <class T>
using line3 = line<3, T>;

template <class T>
using plane2 = plane<2, T>;
template <class T>
using plane3 = plane<3, T>;

//
// Concrete typedefs (2D and 3D; suffix f = f32, d = f64, i = i32)
//

using aabb2f = aabb<2, f32>;
using aabb3f = aabb<3, f32>;
using aabb2d = aabb<2, f64>;
using aabb3d = aabb<3, f64>;
using aabb2i = aabb<2, i32>;
using aabb3i = aabb<3, i32>;

using triangle2f = triangle<2, f32>;
using triangle3f = triangle<3, f32>;
using triangle2d = triangle<2, f64>;
using triangle3d = triangle<3, f64>;
using triangle2i = triangle<2, i32>;
using triangle3i = triangle<3, i32>;

using segment2f = segment<2, f32>;
using segment3f = segment<3, f32>;
using segment2d = segment<2, f64>;
using segment3d = segment<3, f64>;
using segment2i = segment<2, i32>;
using segment3i = segment<3, i32>;

// ray/line/plane carry directions/normals, so only the real-scalar suffixes f/d.
using ray2f = ray<2, f32>;
using ray3f = ray<3, f32>;
using ray2d = ray<2, f64>;
using ray3d = ray<3, f64>;

using line2f = line<2, f32>;
using line3f = line<3, f32>;
using line2d = line<2, f64>;
using line3d = line<3, f64>;

using plane2f = plane<2, f32>;
using plane3f = plane<3, f32>;
using plane2d = plane<2, f64>;
using plane3d = plane<3, f64>;

} // namespace tg
