#pragma once

#include <typed-geometry/geometry/fwd.hh>

/// Object trait seam for typed-geometry's geometric primitives.
///
/// Every geometric object in tg denotes a **set of points** in some surrounding space. The query
/// layer (containment, distance, intersection, …) is phrased uniformly against that set, so each
/// object only has to declare which set it represents — the representation is just the encoding.
/// That declaration lives in tg::object_traits<ObjT>, a primary template each object type
/// specializes in its own header (colocated with the type).
///
/// Three facts describe the set:
///
///   - ambient_dim   — the dimension of the space the points live in (a triangle with 3D coords
///                     has ambient_dim = 3).
///   - intrinsic_dim — the dimension of the set itself as a manifold (a triangle is a 2D surface
///                     patch regardless of the ambient space, so intrinsic_dim = 2). Always
///                     <= ambient_dim; a hyperplane has intrinsic_dim = ambient_dim - 1.
///   - is_finite     — whether the set is bounded. A triangle/segment/aabb is finite; a
///                     ray/line/plane extends to infinity.
///
/// Representation is not interpretation: two objects may share an encoding yet denote different
/// sets. A plane stores {normal, dist} and denotes the points *on* the hyperplane; a future
/// halfspace will reuse the very same {normal, dist} but denote the points on one *side* of it.
///
/// To teach tg about a new object type, specialize tg::object_traits for it next to the type:
///
///     template <int D, class T>
///     struct tg::object_traits<my_object<D, T>>
///     {
///         static constexpr int  intrinsic_dim = ...;
///         static constexpr int  ambient_dim   = D;
///         static constexpr bool is_finite     = ...;
///     };

namespace tg
{
/// Primary template — intentionally left undefined so that every geometric object must opt in by
/// providing its own specialization (a missing one is a hard compile error, not a silent default).
template <class ObjT>
struct object_traits;

namespace traits
{
/// dimension of the object's point set as a manifold (e.g. 2 for a triangle, 1 for a segment).
template <class ObjT>
inline constexpr int intrinsic_dim = object_traits<ObjT>::intrinsic_dim;

/// dimension of the space the object lives in (e.g. 3 for a triangle with 3D coordinates).
template <class ObjT>
inline constexpr int ambient_dim = object_traits<ObjT>::ambient_dim;

/// true if the object's point set is bounded (triangle/segment/aabb) rather than unbounded
/// (ray/line/plane).
template <class ObjT>
inline constexpr bool is_finite = object_traits<ObjT>::is_finite;
} // namespace traits

} // namespace tg
