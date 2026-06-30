#pragma once

#include <clean-core/fwd.hh>

namespace tg
{
// Pull in the shaped-core vocabulary types (i32, f32, isize, ...) so we can write them bare
// inside typed-geometry without leaking them into the global namespace.
using namespace cc::primitive_defines;

//
// Scalar-like types
//

/// a scalar angle (storage is radians); a unit-checked newtype over T.
template <class T>
struct angle;

using angle_f = angle<f32>;
using angle_d = angle<f64>;

//
// Linear algebra types
//

/// neutral component container — D values of T with no geometric semantics.
/// the eventual home of raw, component-wise arithmetic.
template <int D, class T>
struct comp;

/// displacement / direction vector in D dimensions.
template <int D, class T>
struct vec;

/// position / point in D dimensions (a singleton point set).
template <int D, class T>
struct pos;

/// bivector in D dimensions (D*(D-1)/2 components).
template <int D, class T>
struct bivec;

/// column-major matrix with C columns and R rows.
template <int C, int R, class T>
struct mat;

/// quaternion (storage {x, y, z, w}, w is the scalar part).
template <class T>
struct quat;

//
// Dimensional aliases
//

template <class T>
using comp2 = comp<2, T>;
template <class T>
using comp3 = comp<3, T>;
template <class T>
using comp4 = comp<4, T>;

template <class T>
using vec2 = vec<2, T>;
template <class T>
using vec3 = vec<3, T>;
template <class T>
using vec4 = vec<4, T>;

template <class T>
using pos2 = pos<2, T>;
template <class T>
using pos3 = pos<3, T>;
template <class T>
using pos4 = pos<4, T>;

template <class T>
using bivec2 = bivec<2, T>;
template <class T>
using bivec3 = bivec<3, T>;
template <class T>
using bivec4 = bivec<4, T>;

template <class T>
using mat2 = mat<2, 2, T>;
template <class T>
using mat3 = mat<3, 3, T>;
template <class T>
using mat4 = mat<4, 4, T>;

//
// Concrete typedefs (suffix: f = f32, d = f64, i = i32)
//

using comp2f = comp<2, f32>;
using comp3f = comp<3, f32>;
using comp4f = comp<4, f32>;
using comp2d = comp<2, f64>;
using comp3d = comp<3, f64>;
using comp4d = comp<4, f64>;
using comp2i = comp<2, i32>;
using comp3i = comp<3, i32>;
using comp4i = comp<4, i32>;

using vec2f = vec<2, f32>;
using vec3f = vec<3, f32>;
using vec4f = vec<4, f32>;
using vec2d = vec<2, f64>;
using vec3d = vec<3, f64>;
using vec4d = vec<4, f64>;
using vec2i = vec<2, i32>;
using vec3i = vec<3, i32>;
using vec4i = vec<4, i32>;

using pos2f = pos<2, f32>;
using pos3f = pos<3, f32>;
using pos4f = pos<4, f32>;
using pos2d = pos<2, f64>;
using pos3d = pos<3, f64>;
using pos4d = pos<4, f64>;
using pos2i = pos<2, i32>;
using pos3i = pos<3, i32>;
using pos4i = pos<4, i32>;

using bivec2f = bivec<2, f32>;
using bivec3f = bivec<3, f32>;
using bivec4f = bivec<4, f32>;
using bivec2d = bivec<2, f64>;
using bivec3d = bivec<3, f64>;
using bivec4d = bivec<4, f64>;
using bivec2i = bivec<2, i32>;
using bivec3i = bivec<3, i32>;
using bivec4i = bivec<4, i32>;

using mat2f = mat<2, 2, f32>;
using mat3f = mat<3, 3, f32>;
using mat4f = mat<4, 4, f32>;
using mat2d = mat<2, 2, f64>;
using mat3d = mat<3, 3, f64>;
using mat4d = mat<4, 4, f64>;
using mat2i = mat<2, 2, i32>;
using mat3i = mat<3, 3, i32>;
using mat4i = mat<4, 4, i32>;

// quaternions only make sense over real scalars (suffix is `_f`/`_d`: angle/quat end in an
// alpha char, so the suffix is separated; pos3f etc. end in a digit and attach directly).
using quat_f = quat<f32>;
using quat_d = quat<f64>;

} // namespace tg
