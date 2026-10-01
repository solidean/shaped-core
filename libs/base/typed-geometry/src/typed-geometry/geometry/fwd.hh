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

/// half-space {x : dot(normal, x) <= dist}, plane's encoding with the plane as its boundary.
/// Infinite, intrinsic_dim D.
template <int D, class T>
struct halfspace;

/// the faces of an aabb.
/// Finite, intrinsic_dim D-1.
template <int D, class T>
struct aabb_boundary;

/// oriented box {center + H * c : c in [-1, 1]^D}, H's columns the half-axes (not necessarily orthogonal).
/// Finite, intrinsic_dim D; DAmbient is the space it sits in, as for sphere.
template <int D, int DAmbient, class T>
struct box;

/// the faces of a box.
/// Finite, intrinsic_dim D-1.
template <int D, int DAmbient, class T>
struct box_boundary;

/// solid sphere (ball) {x : distance(x, center) <= radius}. Finite, intrinsic_dim D.
/// D is the dimension of the flat it curves in, DAmbient the space that flat sits in.
/// The primary template is undefined — each supported pair is a specialization, since the embedded case also has to name its flat.
template <int D, int DAmbient, class T>
struct sphere;

/// sphere surface {x : distance(x, center) == radius}, the boundary of a sphere.
/// Finite, intrinsic_dim D-1.
template <int D, int DAmbient, class T>
struct sphere_boundary;

/// solid ellipsoid {center + sum_i u_i * semi_axes[i] : |u| <= 1}. Finite, intrinsic_dim D.
/// Same dimension pair as sphere — the D semi-axes span the flat, so nothing else is stored when embedded.
template <int D, int DAmbient, class T>
struct ellipsoid;

/// ellipsoid surface {center + sum_i u_i * semi_axes[i] : |u| == 1}, the boundary of an ellipsoid.
/// Finite, intrinsic_dim D-1.
template <int D, int DAmbient, class T>
struct ellipsoid_boundary;

/// every point within radius of a segment: a stadium in 2D.
/// Finite, intrinsic_dim D.
template <int D, class T>
struct capsule;
/// the surface of a capsule.
template <int D, class T>
struct capsule_boundary;

/// a disk swept along a segment, with flat caps; 3D only.
/// Finite, intrinsic_dim 3.
template <int D, class T>
struct cylinder;
/// the whole surface of a cylinder, caps included.
template <int D, class T>
struct cylinder_boundary;
/// the curved part of a cylinder's surface, open at both ends: a tube.
template <int D, class T>
struct cylinder_mantle;

/// an apex and a base disk of radius at the end of axis from it; 3D only.
/// Finite, intrinsic_dim 3.
template <int D, class T>
struct cone;
/// the whole surface of a cone, base included.
template <int D, class T>
struct cone_boundary;
/// the slanted surface of a cone, without its base.
template <int D, class T>
struct cone_mantle;

/// the half of a ball on the side its unit normal points to, base included; 3D only.
/// Finite, intrinsic_dim 3.
template <int D, class T>
struct hemisphere;
/// the dome and the base of a hemisphere.
template <int D, class T>
struct hemisphere_boundary;
/// the dome of a hemisphere, without its base.
template <int D, class T>
struct hemisphere_mantle;

/// the solid convex hull of four points; 3D only.
/// Finite, intrinsic_dim 3.
template <int D, class T>
struct tetrahedron;
/// the four faces of a tetrahedron.
template <int D, class T>
struct tetrahedron_boundary;

/// the bilinear patch spanned by four corners, which need not be coplanar.
/// Finite, intrinsic_dim 2.
template <int D, class T>
struct quad;

//
// Query results
//

/// how far, and which way, one overlapping solid has to move to leave another: {normal, depth}.
template <int D, class T>
struct separation;

/// at most N crossings of a linear object with a surface, sorted along it.
template <int N, class HitT>
struct hits;

/// the parameters [start, end] of a linear object inside a solid.
template <class T>
struct hit_interval;

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

template <class T>
using halfspace2 = halfspace<2, T>;
template <class T>
using halfspace3 = halfspace<3, T>;

template <class T>
using box2 = box<2, 2, T>;
template <class T>
using box3 = box<3, 3, T>;
template <class T>
using box2in3 = box<2, 3, T>;

template <class T>
using sphere2 = sphere<2, 2, T>;
template <class T>
using sphere3 = sphere<3, 3, T>;

template <class T>
using ellipsoid2 = ellipsoid<2, 2, T>;
template <class T>
using ellipsoid3 = ellipsoid<3, 3, T>;

// a boundary is spelled "_boundary"; in 3D it is also "_surface"
template <class T>
using aabb2_boundary = aabb_boundary<2, T>;
template <class T>
using aabb3_boundary = aabb_boundary<3, T>;
template <class T>
using aabb3_surface = aabb_boundary<3, T>;

template <class T>
using box2_boundary = box_boundary<2, 2, T>;
template <class T>
using box3_boundary = box_boundary<3, 3, T>;
template <class T>
using box3_surface = box_boundary<3, 3, T>;
template <class T>
using box2in3_boundary = box_boundary<2, 3, T>;

template <class T>
using sphere2_boundary = sphere_boundary<2, 2, T>;
template <class T>
using sphere3_boundary = sphere_boundary<3, 3, T>;
template <class T>
using sphere3_surface = sphere_boundary<3, 3, T>;

template <class T>
using ellipsoid2_boundary = ellipsoid_boundary<2, 2, T>;
template <class T>
using ellipsoid3_boundary = ellipsoid_boundary<3, 3, T>;
template <class T>
using ellipsoid3_surface = ellipsoid_boundary<3, 3, T>;

// sphere and ellipsoid also come embedded above their own dimension, spelled "<D>in<DAmbient>":
// a disk or an elliptic patch lying in 3D, and their boundary curves.
template <class T>
using sphere2in3 = sphere<2, 3, T>;
template <class T>
using sphere2in3_boundary = sphere_boundary<2, 3, T>;
template <class T>
using ellipsoid2in3 = ellipsoid<2, 3, T>;
template <class T>
using ellipsoid2in3_boundary = ellipsoid_boundary<2, 3, T>;

// the embedded sphere pair under its everyday names
template <class T>
using disk3 = sphere<2, 3, T>;
template <class T>
using circle3 = sphere_boundary<2, 3, T>;

//
// Concrete typedefs (2D and 3D; suffix f = f32, d = f64, i = i32)
//

using aabb2f = aabb<2, f32>;
using aabb3f = aabb<3, f32>;
using aabb2d = aabb<2, f64>;
using aabb3d = aabb<3, f64>;
using aabb2i = aabb<2, i32>;
using aabb3i = aabb<3, i32>;

using aabb2f_boundary = aabb_boundary<2, f32>;
using aabb3f_boundary = aabb_boundary<3, f32>;
using aabb2d_boundary = aabb_boundary<2, f64>;
using aabb3d_boundary = aabb_boundary<3, f64>;
using aabb2i_boundary = aabb_boundary<2, i32>;
using aabb3i_boundary = aabb_boundary<3, i32>;
using aabb3f_surface = aabb_boundary<3, f32>;
using aabb3d_surface = aabb_boundary<3, f64>;
using aabb3i_surface = aabb_boundary<3, i32>;

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

using halfspace2f = halfspace<2, f32>;
using halfspace3f = halfspace<3, f32>;
using halfspace2d = halfspace<2, f64>;
using halfspace3d = halfspace<3, f64>;

// box carries a half-axis matrix, so only the real-scalar suffixes f/d.
using box2f = box<2, 2, f32>;
using box3f = box<3, 3, f32>;
using box2d = box<2, 2, f64>;
using box3d = box<3, 3, f64>;
using box2in3f = box<2, 3, f32>;
using box2in3d = box<2, 3, f64>;
using box2f_boundary = box_boundary<2, 2, f32>;
using box3f_boundary = box_boundary<3, 3, f32>;
using box2d_boundary = box_boundary<2, 2, f64>;
using box3d_boundary = box_boundary<3, 3, f64>;
using box3f_surface = box_boundary<3, 3, f32>;
using box3d_surface = box_boundary<3, 3, f64>;
using box2in3f_boundary = box_boundary<2, 3, f32>;
using box2in3d_boundary = box_boundary<2, 3, f64>;

// sphere/ellipsoid carry a radius or a semi-axis map, so only the real-scalar suffixes f/d.
using sphere2f = sphere<2, 2, f32>;
using sphere3f = sphere<3, 3, f32>;
using sphere2d = sphere<2, 2, f64>;
using sphere3d = sphere<3, 3, f64>;

using sphere2f_boundary = sphere_boundary<2, 2, f32>;
using sphere3f_boundary = sphere_boundary<3, 3, f32>;
using sphere2d_boundary = sphere_boundary<2, 2, f64>;
using sphere3d_boundary = sphere_boundary<3, 3, f64>;
using sphere3f_surface = sphere_boundary<3, 3, f32>;
using sphere3d_surface = sphere_boundary<3, 3, f64>;

using sphere2in3f = sphere<2, 3, f32>;
using sphere2in3d = sphere<2, 3, f64>;
using sphere2in3f_boundary = sphere_boundary<2, 3, f32>;
using sphere2in3d_boundary = sphere_boundary<2, 3, f64>;

using disk3f = sphere<2, 3, f32>;
using disk3d = sphere<2, 3, f64>;
using circle3f = sphere_boundary<2, 3, f32>;
using circle3d = sphere_boundary<2, 3, f64>;

using ellipsoid2f = ellipsoid<2, 2, f32>;
using ellipsoid3f = ellipsoid<3, 3, f32>;
using ellipsoid2d = ellipsoid<2, 2, f64>;
using ellipsoid3d = ellipsoid<3, 3, f64>;

using ellipsoid2f_boundary = ellipsoid_boundary<2, 2, f32>;
using ellipsoid3f_boundary = ellipsoid_boundary<3, 3, f32>;
using ellipsoid2d_boundary = ellipsoid_boundary<2, 2, f64>;
using ellipsoid3d_boundary = ellipsoid_boundary<3, 3, f64>;
using ellipsoid3f_surface = ellipsoid_boundary<3, 3, f32>;
using ellipsoid3d_surface = ellipsoid_boundary<3, 3, f64>;

using ellipsoid2in3f = ellipsoid<2, 3, f32>;
using ellipsoid2in3d = ellipsoid<2, 3, f64>;
using ellipsoid2in3f_boundary = ellipsoid_boundary<2, 3, f32>;
using ellipsoid2in3d_boundary = ellipsoid_boundary<2, 3, f64>;


template <class T>
using capsule2 = capsule<2, T>;
template <class T>
using capsule3 = capsule<3, T>;
template <class T>
using capsule3_boundary = capsule_boundary<3, T>;
template <class T>
using capsule3_surface = capsule_boundary<3, T>;
template <class T>
using cylinder3 = cylinder<3, T>;
template <class T>
using cylinder3_boundary = cylinder_boundary<3, T>;
template <class T>
using cylinder3_surface = cylinder_boundary<3, T>;
template <class T>
using cylinder3_mantle = cylinder_mantle<3, T>;
template <class T>
using tube3 = cylinder_mantle<3, T>;

using capsule2f = capsule<2, f32>;
using capsule3f = capsule<3, f32>;
using capsule2d = capsule<2, f64>;
using capsule3d = capsule<3, f64>;
using capsule3f_surface = capsule_boundary<3, f32>;
using capsule3d_surface = capsule_boundary<3, f64>;
using cylinder3f = cylinder<3, f32>;
using cylinder3d = cylinder<3, f64>;
using cylinder3f_surface = cylinder_boundary<3, f32>;
using cylinder3d_surface = cylinder_boundary<3, f64>;
using cylinder3f_mantle = cylinder_mantle<3, f32>;
using cylinder3d_mantle = cylinder_mantle<3, f64>;
using tube3f = cylinder_mantle<3, f32>;
using tube3d = cylinder_mantle<3, f64>;


template <class T>
using cone3 = cone<3, T>;
template <class T>
using cone3_boundary = cone_boundary<3, T>;
template <class T>
using cone3_surface = cone_boundary<3, T>;
template <class T>
using cone3_mantle = cone_mantle<3, T>;
template <class T>
using hemisphere3 = hemisphere<3, T>;
template <class T>
using hemisphere3_boundary = hemisphere_boundary<3, T>;
template <class T>
using hemisphere3_surface = hemisphere_boundary<3, T>;
template <class T>
using hemisphere3_mantle = hemisphere_mantle<3, T>;

using cone3f = cone<3, f32>;
using cone3d = cone<3, f64>;
using cone3f_surface = cone_boundary<3, f32>;
using cone3d_surface = cone_boundary<3, f64>;
using cone3f_mantle = cone_mantle<3, f32>;
using cone3d_mantle = cone_mantle<3, f64>;
using hemisphere3f = hemisphere<3, f32>;
using hemisphere3d = hemisphere<3, f64>;
using hemisphere3f_surface = hemisphere_boundary<3, f32>;
using hemisphere3d_surface = hemisphere_boundary<3, f64>;
using hemisphere3f_mantle = hemisphere_mantle<3, f32>;
using hemisphere3d_mantle = hemisphere_mantle<3, f64>;


template <class T>
using tetrahedron3 = tetrahedron<3, T>;
template <class T>
using tetrahedron3_boundary = tetrahedron_boundary<3, T>;
template <class T>
using tetrahedron3_surface = tetrahedron_boundary<3, T>;
template <class T>
using quad2 = quad<2, T>;
template <class T>
using quad3 = quad<3, T>;

using tetrahedron3f = tetrahedron<3, f32>;
using tetrahedron3d = tetrahedron<3, f64>;
using tetrahedron3f_surface = tetrahedron_boundary<3, f32>;
using tetrahedron3d_surface = tetrahedron_boundary<3, f64>;
using quad2f = quad<2, f32>;
using quad3f = quad<3, f32>;
using quad2d = quad<2, f64>;
using quad3d = quad<3, f64>;

} // namespace tg
