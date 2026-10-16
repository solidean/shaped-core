#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/quad.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/linalg/pos.hh>

// tg objects drawn by their parts — the `add_vertices`, `add_edges` and `add_faces` of sv::quadric_set and sv::scene_ref.
//
// An object takes part by what it has: `vertices()`, `edges()` returning segments, `faces()` returning triangles or quads.
// A triangle or a quad is its own one face.
// Every scalar converts to f32 here, so an integer box draws as it is.

namespace sv::impl
{
template <class Obj>
inline constexpr bool is_tg_face = false;
template <class T>
inline constexpr bool is_tg_face<tg::triangle<3, T>> = true;
template <class T>
inline constexpr bool is_tg_face<tg::quad<3, T>> = true;
} // namespace sv::impl

namespace sv
{
/// An object whose corners `add_vertices` can draw.
template <class Obj>
concept has_tg_vertices = requires(Obj const& o) { o.vertices(); };

/// An object whose edges `add_edges` can draw.
template <class Obj>
concept has_tg_edges = requires(Obj const& o) { o.edges(); };

/// An object whose surface `add_faces` can draw: one with `faces()`, or a 3D triangle or quad itself.
template <class Obj>
concept has_tg_faces = impl::is_tg_face<Obj> || requires(Obj const& o) { o.faces(); };
} // namespace sv

namespace sv::impl
{
template <class Array>
struct fixed_size_of;
template <class T, isize N>
struct fixed_size_of<cc::fixed_array<T, N>>
{
    static constexpr isize value = N;
};

template <class T>
[[nodiscard]] constexpr tg::pos3f to_pos3f(tg::pos<3, T> const& p)
{
    return tg::pos3f(float(p.data[0]), float(p.data[1]), float(p.data[2]));
}

/// `obj`'s vertices in f32.
template <class Obj>
[[nodiscard]] constexpr auto vertices_f32(Obj const& obj)
{
    auto const v = obj.vertices();
    cc::fixed_array<tg::pos3f, fixed_size_of<decltype(obj.vertices())>::value> r = {};
    for (isize i = 0; i < v.size(); ++i)
        r[i] = to_pos3f(v[i]);
    return r;
}

/// `obj`'s edges in f32.
template <class Obj>
[[nodiscard]] constexpr auto edges_f32(Obj const& obj)
{
    auto const e = obj.edges();
    cc::fixed_array<tg::segment3f, fixed_size_of<decltype(obj.edges())>::value> r = {};
    for (isize i = 0; i < e.size(); ++i)
        r[i] = tg::segment3f(to_pos3f(e[i].pos0), to_pos3f(e[i].pos1));
    return r;
}

/// Appends the triangles of one face; a quad is split along its pos00-pos11 diagonal, keeping its winding.
template <class Face>
void append_face_triangles(Face const& f, cc::vector<tg::triangle3f>& out)
{
    if constexpr (requires { f.pos00; })
    {
        out.push_back(tg::triangle3f(to_pos3f(f.pos00), to_pos3f(f.pos10), to_pos3f(f.pos11)));
        out.push_back(tg::triangle3f(to_pos3f(f.pos00), to_pos3f(f.pos11), to_pos3f(f.pos01)));
    }
    else
        out.push_back(tg::triangle3f(to_pos3f(f.pos0), to_pos3f(f.pos1), to_pos3f(f.pos2)));
}

/// Appends the triangles of `obj`'s faces, wound as tg winds them.
template <class Obj>
    requires has_tg_faces<Obj>
void append_faces(Obj const& obj, cc::vector<tg::triangle3f>& out)
{
    if constexpr (is_tg_face<Obj>)
        append_face_triangles(obj, out);
    else
        for (auto const& f : obj.faces())
            append_face_triangles(f, out);
}
} // namespace sv::impl
