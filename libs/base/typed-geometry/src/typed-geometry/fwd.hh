#pragma once

#include <clean-core/fwd.hh>

namespace tg
{
// Pull in the shaped-core vocabulary types (i32, f32, isize, ...) so we can write them bare
// inside typed-geometry without leaking them into the global namespace.
using namespace cc::primitive_defines;

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

} // namespace tg
