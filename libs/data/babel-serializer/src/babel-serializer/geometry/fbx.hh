#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/transform.hh>

// Autodesk FBX reader (geometry/), both encodings (binary and ASCII), every version from 6100 on.
//
// The parse is ufbx's, vendored in extern/ufbx and confined to fbx.cc; nothing of it reaches this header.
// What comes back is FBX's own object model, copied out: nodes with their transforms and parents, and meshes as polygons with per-corner attributes.
// Beside them, Phong / Lambert materials as the property bags they are, file textures, and lights.
// Nothing is converted: the axes and the unit the file declares are reported in `settings` and applied to nothing.
//
// Two things are interpreted rather than copied, because the raw format leaves them unusable:
// - A node's local transform is FBX's pivot chain (pre- and post-rotation, rotation and scaling pivots and offsets).
//   It is composed into one translation, rotation and scale, and `node_to_world` also honors the file's scale inheritance mode.
// - FBX's per-layer mapping modes (by polygon vertex, by control point, by polygon, all same; direct or index-to-direct).
//   They are unified into one shape: each attribute is a value array plus one index per face corner.
//
// No triangulation by default (`read_options::triangulate` asks for it), no welding, no pixel decoding, no file I/O.
// A texture is recorded by the file names it carries and by its embedded bytes when it has any; resolving a file name is the caller's business, as babel owns no filesystem policy.
//
// What the reader skipped is recorded in `data::issues` rather than dropped silently: cameras, animation, skinning, blend shapes and non-mesh geometry are all `unsupported` today.
// ufbx's own warnings about tolerated damage (a clamped index, a duplicated connection) arrive as `malformed`.
//
// A READ CAN BE SPREAD OVER A CALLER'S SCHEDULER, AND IT CAN HAND BACK THE SCENE IN TWO STAGES.
// `read_options::scheduler` runs ufbx's parse and babel's copy of each mesh on a `cc::async_scheduler`; null keeps the whole read on the calling thread.
// `read_structure` reads everything but the geometry, which is most of a file, so it returns in a fraction of a full read's time.
// `read_async` runs both side by side and resolves each stage as it finishes, so a caller can fetch textures before the meshes exist.
//
//   auto const doc = babel::fbx::read(stream, {.triangulate = true}).value();
//   for (auto const& n : doc.nodes)
//       if (auto const* m = doc.find(n.mesh); m != nullptr)
//           draw(*m, n.geometry_to_world);

namespace babel::fbx
{
// indices
// -------------------------------------------------------------------------------------------------
// One strong enum per role, `invalid` meaning "the file left it out", as in babel::gltf.

} // namespace babel::fbx

enum class babel::fbx::node_index : int
{
    invalid = -1
};
enum class babel::fbx::mesh_index : int
{
    invalid = -1
};
enum class babel::fbx::material_index : int
{
    invalid = -1
};
enum class babel::fbx::texture_index : int
{
    invalid = -1
};
enum class babel::fbx::light_index : int
{
    invalid = -1
};

// value enums
// -------------------------------------------------------------------------------------------------

/// Which of FBX's two encodings the bytes were in.
enum class babel::fbx::encoding : babel::u8
{
    binary,
    ascii,
};

/// A signed coordinate axis, as FBX's `GlobalSettings` names the scene's right, up and front directions.
enum class babel::fbx::axis : babel::u8
{
    positive_x,
    negative_x,
    positive_y,
    negative_y,
    positive_z,
    negative_z,
};

/// How a node's transform inherits its parent's scale.
/// Only `normal` composes as a plain matrix product; the other two are why `node::node_to_world` exists.
enum class babel::fbx::inherit_mode : babel::u8
{
    /// the parent's whole transform applies (FBX's RSrs)
    normal,
    /// the parent's scale is ignored and only its own applies (Maya's "segment scale compensate", FBX's Rrs)
    ignore_parent_scale,
    /// the parent's scale applies per component, after this node's rotation (FBX's RrSs)
    component_wise_scale,
};

/// A property's FBX type, coarsened to what a reader branches on.
enum class babel::fbx::property_type : babel::u8
{
    other,
    boolean,
    integer,
    number,
    vector,
    color,
    color_with_alpha,
    string,
};

/// A texture's wrapping per axis.
enum class babel::fbx::wrap_mode : babel::u8
{
    repeat,
    clamp,
};

/// A light's `LightType`.
enum class babel::fbx::light_type : babel::u8
{
    point,
    directional,
    spot,
    /// an area light, of the shape `light::area_shape_is_sphere` says
    area,
    volume,
};

/// A light's `DecayType`: how its intensity falls off with distance.
/// FBX's default is `none`, which is what most exporters write whatever the DCC tool did.
enum class babel::fbx::light_decay : babel::u8
{
    none,
    linear,
    quadratic,
    cubic,
};

/// Why the reader recorded an issue; the same three kinds as babel::gltf, with the same rule that an issue never fails the read.
enum class babel::fbx::issue_kind : babel::u8
{
    /// the file uses something this reader does not carry over; nothing is wrong with the file
    unsupported,
    /// a reference the reader could not follow
    unresolved,
    /// the file is damaged in a way the parser tolerated
    malformed,
};

// document elements
// -------------------------------------------------------------------------------------------------

/// One thing the reader noticed and did not fail on; the message names the element, so it can be shown as-is.
struct babel::fbx::issue
{
    issue_kind kind = issue_kind::unsupported;
    cc::string message;
};

/// What wrote the file.
struct babel::fbx::file_info
{
    /// FBX's own version number, e.g. 7500 for FBX 2016
    u32 version = 0;
    fbx::encoding encoding = fbx::encoding::binary;

    /// the `Creator` string, usually the FBX SDK or plugin version
    cc::string creator;
    /// the DCC tool the scene came from ("Maya", "3ds Max", "Blender", ...) and its version
    cc::string application;
    cc::string application_version;
};

/// The scene's `GlobalSettings`: the coordinate system and unit every position in the file is in.
///
/// Read but applied to nothing — a caller wanting meters and Y up composes the change itself, which is what keeps the rest of `data` exactly as the file wrote it.
struct babel::fbx::scene_settings
{
    axis right = axis::positive_x;
    axis up = axis::positive_y;
    axis front = axis::positive_z;

    /// How many meters one unit of the file is: FBX's `UnitScaleFactor` (in centimeters) as meters.
    /// 0.01 is a centimeter scene, the FBX default, and 1 a meter one.
    f64 unit_meters = 0.01;

    /// The unit the authoring tool worked in, which may differ from `unit_meters` when the exporter rescaled.
    f64 original_unit_meters = 0.01;

    f64 frames_per_second = 0;

    /// Whether `right`, `up` and `front` span a left-handed system, which mirrors every mesh relative to a right-handed one.
    [[nodiscard]] bool is_left_handed() const;
};

/// A decomposed affine transform: scale, then rotate, then translate.
struct babel::fbx::transform
{
    tg::vec3f translation = tg::vec3f::zero;
    tg::quat_f rotation = tg::quat_f::identity;
    tg::vec3f scale = tg::vec3f(1, 1, 1);
};

/// One polygon as a run of corners: `[first_corner, first_corner + corner_count)` into every `corner_attribute`'s `indices`.
/// A face with fewer than three corners is a point or a line, which FBX allows in a mesh.
struct babel::fbx::face
{
    i32 first_corner = 0;
    i32 corner_count = 0;
};

/// One vertex attribute in FBX's unified shape: `values`, plus one index into them per face corner.
///
/// FBX stores an attribute by control point, by polygon vertex, by polygon or once for the whole mesh, either directly or through an index array.
/// All of these come back as this one shape, so a reader never branches on a mapping mode.
/// An attribute the mesh does not carry has empty `values` AND empty `indices`.
/// Otherwise `indices.size()` is the mesh's corner count and every index is in range, which the reader guarantees.
template <class T>
struct babel::fbx::corner_attribute
{
    cc::vector<T> values;
    cc::vector<i32> indices;

    [[nodiscard]] bool exists() const { return !indices.empty(); }

    /// the value at face corner `corner`
    [[nodiscard]] T const& at(isize corner) const { return values[isize(indices[corner])]; }
};

/// One UV layer, which FBX names.
///
/// The values are as the file wrote them, with V pointing UP the image — FBX's convention and OpenGL's.
/// A caller sampling a top-row-first image with them flips V.
struct babel::fbx::uv_set
{
    cc::string name;
    corner_attribute<tg::vec2f> uvs;
};

/// One vertex color layer.
struct babel::fbx::color_set
{
    cc::string name;
    corner_attribute<tg::vec4f> colors;
};

/// One FBX `Geometry` of type `Mesh`: polygons over control points, with per-corner attributes.
///
/// A mesh carries no materials of its own.
/// `face_materials` holds per-face SLOT indices, and the materials in those slots belong to the node placing the mesh.
/// So one mesh can draw with different materials on two instances (`data::materials_of`).
struct babel::fbx::mesh
{
    cc::string name;

    cc::vector<face> faces;

    /// per corner, into the control points; always present
    corner_attribute<tg::pos3f> positions;
    corner_attribute<tg::vec3f> normals;
    /// the tangents and bitangents of the FIRST uv set, which is the only one FBX gives them for in practice
    corner_attribute<tg::vec3f> tangents;
    corner_attribute<tg::vec3f> bitangents;

    cc::vector<uv_set> uv_sets;
    cc::vector<color_set> color_sets;

    /// One material slot per face, or empty when the mesh uses a single slot for every face.
    /// A slot is an index into the placing node's materials, not into `data::materials`.
    cc::vector<i32> face_materials;

    /// The slot face `f` draws with: `face_materials[f]`, or 0 for a single-slot mesh.
    [[nodiscard]] i32 material_slot_of(isize f) const { return face_materials.empty() ? 0 : face_materials[f]; }

    /// how many corners the faces span, which is what every attribute's `indices` is sized by
    [[nodiscard]] isize corner_count() const { return positions.indices.size(); }
};

/// One property of an object's property bag, as FBX typed it.
/// `value` holds a number in `x` and a vector or color in its first components; `text` holds a string property.
struct babel::fbx::property
{
    cc::string name;
    property_type type = property_type::other;
    tg::vec4f value = tg::vec4f::zero;
    i64 value_int = 0;
    cc::string text;
};

/// One texture connected to a material property — FBX connects a texture to a property by name rather than to a slot.
struct babel::fbx::material_texture
{
    /// the property the texture drives: "DiffuseColor", "NormalMap", "SpecularColor", "EmissiveColor", ...
    cc::string property;
    texture_index texture = texture_index::invalid;
};

/// One FBX `Material`: a shading model name over a bag of properties, with textures connected to some of them.
///
/// Kept as the bag it is rather than mapped onto a material model, because FBX's Phong and Lambert properties mean whatever the exporting pipeline made them mean.
/// A renderer that reads `SpecularColor` as a packed occlusion-roughness-metalness map — the convention NVIDIA's ORCA scenes ship under — reads it from here exactly as one reading Phong does.
struct babel::fbx::material
{
    cc::string name;

    /// the `ShadingModel` as written, lowercased by convention: "phong", "lambert", or a renderer's own
    cc::string shading_model;

    cc::vector<property> properties;
    cc::vector<material_texture> textures;

    /// The property named `name`, or nullptr when the material does not carry it.
    [[nodiscard]] property const* find_property(cc::string_view name) const;

    /// The texture connected to the property named `name`, or `invalid` when none is.
    [[nodiscard]] texture_index find_texture(cc::string_view property_name) const;
};

/// One FBX `Texture` of a file, with what it refers to.
///
/// File names arrive exactly as the file carries them, separators included — an FBX written on Windows says `Textures\wood.dds`.
/// `absolute_filename` is wherever the file lived on the exporting machine, which is rarely anywhere now.
struct babel::fbx::texture
{
    cc::string name;
    cc::string relative_filename;
    cc::string absolute_filename;

    /// The embedded copy of the image, when the file carries one; empty otherwise.
    /// Textures embedding one blob SHARE it: the pin is copied out of the file once, so the result never holds more embedded bytes than the input did.
    cc::pinned_data<byte const> content;

    wrap_mode wrap_u = wrap_mode::repeat;
    wrap_mode wrap_v = wrap_mode::repeat;

    /// the uv set this texture samples, by name; empty means the mesh's first
    cc::string uv_set;

    /// Whether the texture carries a uv transform (translation, rotation or scale), which `uv_transform` holds.
    bool has_uv_transform = false;
    transform uv_transform;

    /// false for a layered, procedural or shader texture, which carries no file of its own
    bool is_file = true;
};

/// One FBX `NodeAttribute` of type `Light`.
///
/// A light has no position of its own: the node placing it gives one, and it shines along `local_direction` in that node's space.
/// FBX's `Intensity` is a percentage — 100 is the DCC tool's 1 — so `intensity` is that property over 100, in no physical unit at all.
struct babel::fbx::light
{
    cc::string name;
    light_type type = light_type::point;
    light_decay decay = light_decay::none;

    /// linear RGB
    tg::vec3f color = tg::vec3f(1, 1, 1);
    f32 intensity = 1;

    /// the direction the light shines along in its node's space; FBX's convention is -Y
    tg::vec3f local_direction = tg::vec3f(0, -1, 0);

    /// spot only: the FULL cone angles in degrees, inner fully lit and outer cut off
    f32 inner_angle_degrees = 0;
    f32 outer_angle_degrees = 45;

    /// area only: true for a sphere, false for a rectangle
    bool area_shape_is_sphere = false;

    bool cast_light = true;
    bool cast_shadows = true;
};

/// One FBX `Model`: a place in the hierarchy with what it carries.
///
/// `local` is the node's transform relative to its parent, FBX's full pivot chain composed by the reader.
/// `geometry` is FBX's geometric transform, which moves the node's mesh but NOT its children, and not its light either.
/// A light is placed by `node_to_world`, as ufbx and the FBX SDK place one.
/// `node_to_world` and `geometry_to_world` are the composed results, with the file's scale inheritance honored.
/// A plain product of `local` transforms does not honor it for a node whose `inherit` is not `normal`.
struct babel::fbx::node
{
    cc::string name;
    node_index parent = node_index::invalid;

    i32 first_child = 0; ///< run in data.node_children
    i32 child_count = 0;

    transform local;
    transform geometry;
    bool has_geometry_transform = false;
    fbx::inherit_mode inherit = inherit_mode::normal;

    tg::affine_transform3f node_to_world = {};
    tg::affine_transform3f geometry_to_world = {};

    mesh_index mesh = mesh_index::invalid;
    light_index light = light_index::invalid;

    /// The node's material slots, which a mesh's `face_materials` index: run in data.node_materials.
    i32 first_material = 0;
    i32 material_count = 0;

    /// FBX's `Visibility`: false marks helper or proxy geometry the scene's author hid.
    bool visible = true;
};

// the document
// -------------------------------------------------------------------------------------------------

/// The parse of one FBX file.
///
/// `nodes` is in PARENTS-FIRST order: a node's parent always precedes it, so one forward pass can accumulate transforms, and `nodes[0]` is the scene's root, which FBX gives no name and no transform.
/// Every other vector is in the order the file declared its objects.
struct babel::fbx::data
{
    file_info info;
    scene_settings settings;

    /// Everything the reader skipped or tolerated, in the order it was noticed.
    /// A successful read with a non-empty `issues` is the normal case for a real-world file.
    cc::vector<issue> issues;

    cc::vector<node> nodes;
    cc::vector<node_index> node_children;      ///< every node's children, flattened
    cc::vector<material_index> node_materials; ///< every node's material slots, flattened; `invalid` for an empty slot

    cc::vector<mesh> meshes;
    cc::vector<material> materials;
    cc::vector<texture> textures;
    cc::vector<light> lights;

    // import issues
public:
    [[nodiscard]] bool has_issues() const { return !issues.empty(); }
    [[nodiscard]] bool has_issue_of(issue_kind kind) const;

    /// Every recorded issue joined into one newline-separated block, empty when there are none.
    [[nodiscard]] cc::string issue_report() const;

    // index lookup
public:
    /// The element a typed index designates, or nullptr for `invalid`.
    /// The reader validates every index it stores, so nullptr always means "the file left this out".
    [[nodiscard]] node const* find(node_index i) const { return impl_find(nodes, i); }
    [[nodiscard]] mesh const* find(mesh_index i) const { return impl_find(meshes, i); }
    [[nodiscard]] material const* find(material_index i) const { return impl_find(materials, i); }
    [[nodiscard]] texture const* find(texture_index i) const { return impl_find(textures, i); }
    [[nodiscard]] light const* find(light_index i) const { return impl_find(lights, i); }

    // flattened runs
public:
    [[nodiscard]] cc::span<node_index const> children_of(node const& n) const
    {
        return cc::span<node_index const>(node_children)
            .subspan({.offset = isize(n.first_child), .size = isize(n.child_count)});
    }

    /// The node's material slots, indexed by a mesh's `face_materials`.
    [[nodiscard]] cc::span<material_index const> materials_of(node const& n) const
    {
        return cc::span<material_index const>(node_materials)
            .subspan({.offset = isize(n.first_material), .size = isize(n.material_count)});
    }

    // implementation
private:
    template <class Element, class Index>
    [[nodiscard]] static Element const* impl_find(cc::vector<Element> const& elements, Index i)
    {
        auto const raw = isize(int(i));
        return raw >= 0 && raw < elements.size() ? &elements[raw] : nullptr;
    }
};

/// Reader knobs; every default leaves the file's shape alone.
struct babel::fbx::read_options
{
    /// Split every polygon into triangles, ear-clipping a concave one rather than fanning it.
    /// Faces with fewer than three corners are dropped, and each triangle keeps its polygon's material slot.
    /// So is a face of more than 4096 corners, with an issue: the ear clip is quadratic in a face's size, and no modeling tool writes one that large.
    /// Corners are copied rather than re-indexed, so every attribute keeps its `values` and only its `indices` grow.
    bool triangulate = false;

    /// The most memory ufbx may hold while parsing, in bytes: half for its working memory, half for the scene it builds.
    /// The copy into `data` comes on top, and is about the size of that scene.
    /// A binary FBX array states its element count before its compressed bytes, and ufbx allocates that count first.
    /// So without a ceiling a few crafted bytes can ask for tens of gigabytes.
    /// 0 picks one from the input: 32 times its size plus 64 MiB, capped at 4 GiB.
    /// Bistro's 114 MB exterior builds a 630 MB scene, under its 1.8 GB half.
    /// A plain stream cannot say how long it is, so it gets 1 GiB, which a scene that size exceeds: read a large file through the span overload, or pass a limit.
    /// Exceeding it is a read error, never a truncated scene.
    i64 memory_limit = 0;

    /// Where the read's parallel work runs: ufbx's parse, and babel's copy of each mesh.
    /// Null keeps the whole read on the calling thread, one step after another.
    /// The calling thread takes part either way, and the result is the same field for field, issues in the same order.
    /// A pool such as the installed `cc::compute_scheduler()` is what it is for; the scheduler must outlive the read.
    cc::async_scheduler* scheduler = nullptr;
};

/// The two stages of `read_async`: the scene's structure first, then all of it.
struct babel::fbx::loading
{
    /// `read_structure`'s result: every node, material, texture and light, and every mesh named and empty.
    cc::shared_async<data> structure;

    /// `read`'s result, whose indices match `structure`'s.
    cc::shared_async<data> complete;
};

namespace babel::fbx
{
// reading
// -------------------------------------------------------------------------------------------------

/// Parse an FBX file from a stream, in either encoding.
/// The stream is read front to back and never seeked; ufbx keeps what it needs in memory as it goes.
/// Fails on anything that is not FBX, and on a file too damaged to yield a scene.
/// Also on one that needs more than `read_options::memory_limit`, and on a mesh whose counts do not fit the `i32` indices stored here.
[[nodiscard]] cc::result<data> read(cc::read_stream& in, read_options opts = {});

/// Convenience: parse from an in-memory buffer (wraps a span_read_stream_adapter).
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes, read_options opts = {});
[[nodiscard]] cc::result<data> read(cc::string_view text, read_options opts = {});

/// Parse an FBX file without its geometry: nodes and their transforms, materials, textures, lights and settings.
/// Every mesh is there and named, at the index the full read gives it, and holds no faces and no attributes.
/// `issues` holds what this pass noticed, which leaves out what only the geometry shows.
/// Vertex data is most of a file, so this returns in a fraction of a full read's time — what a caller needs to start fetching textures.
[[nodiscard]] cc::result<data> read_structure(cc::read_stream& in, read_options opts = {});
[[nodiscard]] cc::result<data> read_structure(cc::span<byte const> bytes, read_options opts = {});

/// Read an FBX file on a scheduler, handing back each stage as soon as it exists.
/// Both stages are scheduled before this returns, on `opts.scheduler`, or on `cc::compute_scheduler()` when that is null, and they run side by side.
/// Each is its own parse of `bytes`: `structure` is `read_structure`'s and resolves first, `complete` is `read`'s.
/// The input is pinned because the read outlives this call, and dropping the stages does not stop it.
/// A file a stage cannot read fails that stage with the error its synchronous call returns.
[[nodiscard]] loading read_async(cc::pinned_data<byte const> bytes, read_options opts = {});
} // namespace babel::fbx
