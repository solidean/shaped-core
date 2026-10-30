#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Wavefront OBJ reader (geometry/).
//
// A faithful, flat mirror of the file: parallel attribute arrays exactly as listed, plus faces as runs of corners.
// NO triangulation, NO vertex dedup, NO mesh building — that belongs in a higher-level "load a mesh" aggregator,
// which wants a tg::mesh that is not built yet.
// All indices are resolved to 0-based here (OBJ's 1-based and negative/relative forms are both applied),
// with -1 meaning "attribute absent" on a corner.
//
// Parses line by line off a cc::read_stream (read_stream::read_line) — it never buffers the whole file.
//
//   auto m = babel::obj::read(source).value();
//   for (auto const& f : m.faces)
//       for (auto ci = f.first_corner; ci < f.first_corner + f.corner_count; ++ci)
//           auto const p = m.positions[m.corners[ci].position];
//
// The MTL files an OBJ names (`mtllib`) are a format of their own, which read_materials parses the same faithful way:
// each `newmtl` block with the statements it holds, and nothing applied.

/// One face corner: 0-based indices into positions / texcoords / normals; -1 when that attribute is absent.
struct babel::obj::corner
{
    i32 position = -1;
    i32 texcoord = -1;
    i32 normal = -1;
};

/// A face as a contiguous run of corners in data.corners: [first_corner, first_corner + corner_count).
/// Polygons of any arity are preserved (not triangulated).
struct babel::obj::face
{
    i32 first_corner = 0;
    i32 corner_count = 0;
};

/// A named span over faces: [first_face, first_face + face_count). Used for `o` / `g` names and `usemtl`
/// material assignment — recorded, not applied.
struct babel::obj::group
{
    cc::string name;
    i32 first_face = 0;
    i32 face_count = 0;
};

/// The faithful parse of a Wavefront .obj.
/// Read-once; the vectors mirror the file's declaration order.
struct babel::obj::data
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

/// One texture statement of an MTL material (`map_Kd`, `bump`, ...): the file it names, and the options this reader models.
///
/// The file name is the rest of the statement after its options, spaces included, exactly as written.
/// IT IS UNTRUSTED INPUT: exporters write absolute paths from the machine they ran on, with either separator, and a name may climb out with `..`.
/// Resolving or confining it is the caller's business, as babel owns no filesystem policy.
/// Options the reader does not model (-blendu, -mm, -texres, ...) are skipped, and so are their arguments.
struct babel::obj::texture_map
{
    cc::string filename;
    tg::vec3f offset = tg::vec3f(0, 0, 0); // -o
    tg::vec3f scale = tg::vec3f(1, 1, 1);  // -s
    f32 bump_multiplier = 1;               // -bm
    bool clamp = false;                    // -clamp on
};

/// One `newmtl` block of an MTL file, as written: a statement the file left out stays empty, so a caller applies its own defaults.
/// Colors are the numbers the file holds, which exporters mean as sRGB more often than not; the reader converts nothing.
struct babel::obj::material
{
    cc::string name;

    cc::optional<tg::vec3f> ambient;             // Ka
    cc::optional<tg::vec3f> diffuse;             // Kd
    cc::optional<tg::vec3f> specular;            // Ks
    cc::optional<tg::vec3f> emissive;            // Ke
    cc::optional<tg::vec3f> transmission_filter; // Tf
    cc::optional<f32> specular_exponent;         // Ns
    cc::optional<f32> dissolve;                  // d: 1 is opaque
    cc::optional<f32> transparency;              // Tr: 1 - d, which some exporters write instead
    cc::optional<f32> optical_density;           // Ni
    cc::optional<i32> illumination_model;        // illum
    cc::optional<f32> roughness;                 // Pr, from the PBR extension
    cc::optional<f32> metallic;                  // Pm, from the PBR extension

    cc::optional<texture_map> ambient_map;           // map_Ka
    cc::optional<texture_map> diffuse_map;           // map_Kd
    cc::optional<texture_map> specular_map;          // map_Ks
    cc::optional<texture_map> emissive_map;          // map_Ke
    cc::optional<texture_map> specular_exponent_map; // map_Ns
    cc::optional<texture_map> dissolve_map;          // map_d
    cc::optional<texture_map> bump_map;              // bump, map_bump
    cc::optional<texture_map> displacement_map;      // disp
    cc::optional<texture_map> normal_map;            // norm
    cc::optional<texture_map> roughness_map;         // map_Pr
    cc::optional<texture_map> metallic_map;          // map_Pm
};

namespace babel::obj
{

// reading
// -------------------------------------------------------------------------------------------------

/// Parse a complete OBJ document from a stream.
/// Unknown directives (incl. `s`) are skipped, and blank lines and `#` comments are ignored.
/// Fails with a line-numbered error on a malformed vertex / face.
[[nodiscard]] cc::result<data> read(cc::read_stream& in);

/// Convenience: parse from an in-memory buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<data> read(cc::string_view text);
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes);

/// Parse an MTL file: its materials in the order the file defines them.
/// Unknown statements are skipped, and so is a statement before the first `newmtl` and a color given as `spectral` or `xyz`.
/// A color given as one number is that number in every channel, as the format specifies.
/// Fails with a line-numbered error on a malformed number.
[[nodiscard]] cc::result<cc::vector<material>> read_materials(cc::read_stream& in);
[[nodiscard]] cc::result<cc::vector<material>> read_materials(cc::string_view text);
[[nodiscard]] cc::result<cc::vector<material>> read_materials(cc::span<byte const> bytes);
} // namespace babel::obj
