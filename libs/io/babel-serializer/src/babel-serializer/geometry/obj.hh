#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Wavefront OBJ reader (geometry/).
//
// A faithful, flat mirror of the file: parallel attribute arrays exactly as listed, plus faces as runs of
// corners. NO triangulation, NO vertex dedup, NO mesh building — that belongs in a higher-level "load a mesh"
// aggregator (which wants a tg::mesh; not built yet). All indices are resolved to 0-based here (OBJ's 1-based
// and negative/relative forms are both applied), with -1 meaning "attribute absent" on a corner.
//
// Parses line by line off a cc::read_stream (cc::read_line) — it never buffers the whole file.
//
//   auto m = babel::obj::read(source).value();
//   for (auto const& f : m.faces)
//       for (auto ci = f.first_corner; ci < f.first_corner + f.corner_count; ++ci)
//           auto const p = m.positions[m.corners[ci].position];

namespace babel::obj
{
/// One face corner: 0-based indices into positions / texcoords / normals; -1 when that attribute is absent.
struct corner
{
    i32 position = -1;
    i32 texcoord = -1;
    i32 normal = -1;
};

/// A face as a contiguous run of corners in data.corners: [first_corner, first_corner + corner_count).
/// Polygons of any arity are preserved (not triangulated).
struct face
{
    i32 first_corner = 0;
    i32 corner_count = 0;
};

/// A named span over faces: [first_face, first_face + face_count). Used for `o` / `g` names and `usemtl`
/// material assignment — recorded, not applied.
struct group
{
    cc::string name;
    i32 first_face = 0;
    i32 face_count = 0;
};

/// The faithful parse of a Wavefront .obj. Read-once; the vectors mirror the file's declaration order.
struct data
{
    cc::vector<tg::pos3f> positions; // v  (optional w is dropped)
    cc::vector<tg::vec2f> texcoords; // vt (u, v; an optional third coord is dropped)
    cc::vector<tg::vec3f> normals;   // vn

    cc::vector<corner> corners; // every face corner, flattened
    cc::vector<face> faces;     // each face is a run of corners

    cc::vector<group> objects;   // `o` object spans over faces
    cc::vector<group> groups;    // `g` group spans over faces
    cc::vector<group> materials; // `usemtl` material spans over faces

    cc::vector<cc::string> material_libraries; // `mtllib` referenced files
};

// reading
// -------------------------------------------------------------------------------------------------

/// Parse a complete OBJ document from a stream. Unknown directives (incl. `s`) are skipped; blank lines and
/// `#` comments are ignored. Fails with a line-numbered error on a malformed vertex / face.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

/// Convenience: parse from an in-memory buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<data> read(cc::string_view text);
[[nodiscard]] cc::result<data> read(cc::span<cc::byte const> bytes);
} // namespace babel::obj
