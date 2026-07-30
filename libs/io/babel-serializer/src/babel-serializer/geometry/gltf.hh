#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/strided_span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>

#include <type_traits>

// glTF 2.0 reader (geometry/), for both the JSON `.gltf` and the binary `.glb` container.
//
// A faithful, flat mirror of the document: every glTF array becomes a cc::vector of the matching struct,
// cross-references keep the file's index shape (as typed indices), and every "list of lists" is flattened
// into one array plus per-owner runs — the same shape babel::obj uses for face corners.
// NO mesh building, NO triangulation, NO pixel decoding, NO flattening of the node hierarchy.
//
// UNLIKE every other babel reader this one takes bytes, not a cc::read_stream, and zero copy is the reason:
// a .glb carries its vertex / index / texture payload inline, so each buffer comes back as a
// cc::pinned_data SUBVIEW of the input that shares its owner. No bulk data is copied, and the views stay
// valid after the caller drops its own handle.
// The JSON part costs nothing extra — the JSON chunk is a subspan handed to babel::json::read, whose span
// stream is unbuffered, so the parse runs directly against the input bytes.
//
// Reading is strict where bytes are at stake and lenient everywhere else: an unknown componentType, a sparse
// accessor and a non-empty extensionsRequired all fail the read, while unknown members, morph targets and the
// skin / animation / camera arrays are skipped.
// Skipped is not silent — everything the reader did not implement, could not resolve, or chose to tolerate is
// recorded in `data::issues`. A successful read with a non-empty issue list is the normal case for a
// real-world asset, so check it before assuming you got everything the file described.
//
//   auto const doc = babel::gltf::read(bytes).value();
//   for (auto const& prim : doc.primitives_of(doc.meshes[0]))
//   {
//       auto const view = doc.view_of(doc.find_attribute(prim, "POSITION")).value();
//       for (auto const& p : view.read_elements<tg::vec3f>().value())
//           use(p);
//   }

namespace babel::gltf
{
// indices
// -------------------------------------------------------------------------------------------------
// glTF is index-heavy and its indices are bare JSON integers, so an accessor index and a material index
// would silently swap as plain ints. One strong enum per role, `invalid` meaning "the file left it out".
// Cross into the underlying int with an explicit `int(x)` at the use site.

enum class buffer_index : int
{
    invalid = -1
};
enum class buffer_view_index : int
{
    invalid = -1
};
enum class accessor_index : int
{
    invalid = -1
};
enum class mesh_index : int
{
    invalid = -1
};
enum class node_index : int
{
    invalid = -1
};
enum class scene_index : int
{
    invalid = -1
};
enum class material_index : int
{
    invalid = -1
};
enum class texture_index : int
{
    invalid = -1
};
enum class image_index : int
{
    invalid = -1
};
enum class sampler_index : int
{
    invalid = -1
};

// value enums
// -------------------------------------------------------------------------------------------------
// The enumerator values are the spec's own numbers, so casting back to the underlying type recovers
// exactly what the file said.

/// Which container the bytes were in.
enum class container : u8
{
    gltf, // a JSON document
    glb,  // the binary container
};

/// An accessor's `componentType`.
enum class component_type : u16
{
    invalid = 0,
    i8 = 5120,
    u8 = 5121,
    i16 = 5122,
    u16 = 5123,
    u32 = 5125,
    f32 = 5126,
};

/// An accessor's `type`, i.e. how many components make one element.
enum class accessor_type : u8
{
    invalid,
    scalar,
    vec2,
    vec3,
    vec4,
    mat2,
    mat3,
    mat4,
};

/// A primitive's topology.
enum class primitive_mode : u8
{
    points = 0,
    lines = 1,
    line_loop = 2,
    line_strip = 3,
    triangles = 4,
    triangle_strip = 5,
    triangle_fan = 6,
};

/// A bufferView's declared GPU binding; `none` when the file did not state one.
enum class buffer_target : u16
{
    none = 0,
    array_buffer = 34962,
    element_array_buffer = 34963,
};

/// A material's `alphaMode`.
enum class alpha_mode : u8
{
    opaque,
    mask,
    blend,
};

/// A sampler's magnification / minification filter; `none` means unspecified (the runtime chooses).
/// The mipmap variants are only valid as a minification filter.
enum class filter : u16
{
    none = 0,
    nearest = 9728,
    linear = 9729,
    nearest_mipmap_nearest = 9984,
    linear_mipmap_nearest = 9985,
    nearest_mipmap_linear = 9986,
    linear_mipmap_linear = 9987,
};

/// A sampler's wrapping mode per texture axis.
enum class wrap_mode : u16
{
    repeat = 10497,
    clamp_to_edge = 33071,
    mirrored_repeat = 33648,
};

// document elements
// -------------------------------------------------------------------------------------------------

/// The document's `asset` block. Named asset_info because `data` already has a member called `asset`.
/// `version` is the one property glTF requires of every file.
struct asset_info
{
    cc::string version;     // e.g. "2.0"
    cc::string min_version; // "minVersion"; empty when absent
    cc::string generator;
    cc::string copyright;
};

/// One `buffers` entry, with its bytes resolved when they were reachable.
/// A GLB's BIN chunk and a base64 `data:` URI both resolve during read; an external URI needs
/// read_options::resolve_uri, and stays unresolved without it.
struct buffer
{
    cc::string uri;      // exactly as written; empty for the GLB BIN chunk
    i64 byte_length = 0; // the declared byteLength
    /// The buffer's bytes; a zero-copy subview of the read input whenever they came from it.
    /// resolved implies data.size() == byte_length, so a caller never has to re-check the length.
    cc::pinned_data<byte const> data;
    bool resolved = false; // false with empty data means "an external URI nobody fetched"
};

/// One `bufferViews` entry: a window into a buffer, optionally with a vertex stride.
struct buffer_view
{
    buffer_index buffer = buffer_index::invalid;
    i64 byte_offset = 0;
    i64 byte_length = 0;
    /// The byte distance between consecutive elements, or 0 when the file omitted it (tightly packed).
    /// 0 is never a real stride — accessor_view resolves it to the element size.
    i64 byte_stride = 0;
    buffer_target target = buffer_target::none;
    cc::string name;
};

/// One `accessors` entry: typed elements inside a bufferView.
/// Sparse accessors are rejected by read, so a parsed accessor is always dense.
struct accessor
{
    buffer_view_index buffer_view = buffer_view_index::invalid; // invalid == the spec's implicit all-zero data
    i64 byte_offset = 0;                                        // relative to the bufferView's own byte_offset
    component_type component = component_type::invalid;
    accessor_type type = accessor_type::invalid;
    i64 count = 0;
    bool normalized = false;

    /// The `min` values then the `max` values, in data.accessor_bounds[first_bound, first_bound + bound_count).
    /// bound_count is either 0 (the file stated neither) or 2 * component_count().
    i32 first_bound = 0;
    i32 bound_count = 0;

    cc::string name;

    /// Components per element: 1 / 2 / 3 / 4 / 4 / 9 / 16 by `type`.
    [[nodiscard]] int component_count() const
    {
        switch (type)
        {
        case accessor_type::scalar:
            return 1;
        case accessor_type::vec2:
            return 2;
        case accessor_type::vec3:
            return 3;
        case accessor_type::vec4:
        case accessor_type::mat2:
            return 4;
        case accessor_type::mat3:
            return 9;
        case accessor_type::mat4:
            return 16;
        default:
            return 0;
        }
    }

    /// Bytes per component: 1 / 1 / 2 / 2 / 4 / 4 by `component`.
    [[nodiscard]] int component_size() const
    {
        switch (component)
        {
        case component_type::i8:
        case component_type::u8:
            return 1;
        case component_type::i16:
        case component_type::u16:
            return 2;
        case component_type::u32:
        case component_type::f32:
            return 4;
        default:
            return 0;
        }
    }

    /// Bytes one tightly-packed element occupies, INCLUDING the spec's per-column 4-byte padding of the
    /// matrix types: a mat3 of u8 is 12 bytes, not 9, and a mat3 of u16 is 24, not 18.
    [[nodiscard]] i64 element_size() const
    {
        auto const cs = i64(component_size());
        auto const columns = [this]() -> i64
        {
            switch (type)
            {
            case accessor_type::mat2:
                return 2;
            case accessor_type::mat3:
                return 3;
            case accessor_type::mat4:
                return 4;
            default:
                return 0;
            }
        }();

        if (columns == 0)
            return i64(component_count()) * cs;

        auto const column_bytes = (columns * cs + 3) / 4 * 4; // each matrix column starts on a 4-byte boundary
        return columns * column_bytes;
    }
};

/// One `attributes` entry of a primitive. The semantic is kept verbatim, custom `_NAME` ones included.
struct attribute
{
    cc::string semantic; // "POSITION", "NORMAL", "TEXCOORD_0", "_CUSTOM", ...
    accessor_index accessor = accessor_index::invalid;
};

/// One primitive of a mesh: a run of attributes plus the optional index and material references.
struct primitive
{
    i32 first_attribute = 0; // run in data.attributes
    i32 attribute_count = 0;
    accessor_index indices = accessor_index::invalid;  // invalid == a non-indexed draw
    material_index material = material_index::invalid; // invalid == the spec's default material
    primitive_mode mode = primitive_mode::triangles;
};

/// One `meshes` entry: a run of primitives.
struct mesh
{
    i32 first_primitive = 0; // run in data.primitives
    i32 primitive_count = 0;
    cc::string name;
};

/// One `nodes` entry of the scene graph.
struct node
{
    i32 first_child = 0; // run in data.node_children
    i32 child_count = 0;
    mesh_index mesh = mesh_index::invalid;

    /// The local transform, kept in whichever form the file used — the spec makes `matrix` and TRS mutually
    /// exclusive, and this reader does not pick one for you.
    /// has_matrix true: `matrix` is authoritative and the TRS fields are at their defaults.
    /// has_matrix false: the TRS fields are authoritative and `matrix` is the identity.
    bool has_matrix = false;
    tg::mat4f matrix = tg::mat4f::identity;
    tg::vec3f translation = tg::vec3f::zero;
    tg::quat_f rotation = tg::quat_f::identity;
    tg::vec3f scale = tg::vec3f(1, 1, 1);

    cc::string name;
};

/// One `scenes` entry: a run of root nodes.
struct scene
{
    i32 first_node = 0; // run in data.scene_nodes
    i32 node_count = 0;
    cc::string name;
};

/// A material's reference to a texture. `texture == invalid` means the reference itself is absent.
struct texture_ref
{
    texture_index texture = texture_index::invalid;
    i32 texcoord = 0; // the n in TEXCOORD_n
};

/// One `materials` entry, core metallic-roughness only.
/// The KHR_materials_* extensions are not interpreted; their raw JSON is skipped.
struct material
{
    tg::vec4f base_color_factor = tg::vec4f(1, 1, 1, 1);
    f32 metallic_factor = 1;
    f32 roughness_factor = 1;
    texture_ref base_color_texture;
    texture_ref metallic_roughness_texture;

    texture_ref normal_texture;
    f32 normal_scale = 1; // normalTexture.scale
    texture_ref occlusion_texture;
    f32 occlusion_strength = 1; // occlusionTexture.strength
    texture_ref emissive_texture;
    tg::vec3f emissive_factor = tg::vec3f::zero;

    alpha_mode alpha = alpha_mode::opaque;
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;

    cc::string name;
};

/// One `textures` entry: an image plus how to sample it.
struct texture
{
    sampler_index sampler = sampler_index::invalid; // invalid == the spec's default sampler
    image_index source = image_index::invalid;
    cc::string name;
};

/// One `images` entry, holding the image's ENCODED bytes — babel::gltf never decodes pixels.
/// Hand `data` to babel::image::read when you want them.
struct image
{
    cc::string uri;       // exactly as written; empty for a bufferView-backed image
    cc::string mime_type; // "image/png" / "image/jpeg"; may be empty for a URI image
    buffer_view_index buffer_view = buffer_view_index::invalid;
    /// The encoded bytes; a zero-copy subview of the read input for a bufferView-backed image.
    cc::pinned_data<byte const> data;
    bool resolved = false;
    cc::string name;
};

/// One `samplers` entry.
struct sampler
{
    filter mag_filter = filter::none;
    filter min_filter = filter::none;
    wrap_mode wrap_s = wrap_mode::repeat;
    wrap_mode wrap_t = wrap_mode::repeat;
    cc::string name;
};

// accessor data
// -------------------------------------------------------------------------------------------------

/// A resolved, strided view of one accessor's elements, carrying the buffer's pin.
/// `bytes` starts at the accessor's FIRST element and spans exactly (count - 1) * stride + element_size
/// bytes, so element(count - 1) is the last valid read.
/// Zero-copy: `bytes` is a subview of the buffer, so a view may outlive the `data` it came from.
///
///   auto const v = doc.view_of(a).value();
///   if (v.is_typed_as<tg::vec3f>())
///       for (auto const& p : v.as_strided<tg::vec3f>()) use(p);
///   else
///       for (auto const& p : v.read_elements<tg::vec3f>().value()) use(p);
struct accessor_view
{
    cc::pinned_data<byte const> bytes;
    i64 stride = 0;       // byte distance between elements; NEVER 0 (a packed accessor gets element_size)
    i64 count = 0;        // number of elements
    i64 element_size = 0; // bytes per element, matrix column padding included
    component_type component = component_type::invalid;
    accessor_type type = accessor_type::invalid;
    bool normalized = false;

    /// The i-th element's bytes. Non-owning, valid as long as THIS view lives (it holds the pin).
    /// Deliberately not a pinned_data: that would be a refcount bump per element.
    /// Precondition: 0 <= i < count.
    [[nodiscard]] cc::span<byte const> element(i64 i) const
    {
        CC_ASSERT(0 <= i && i < count, "accessor element index out of range");
        return bytes.span().subspan({.offset = i * stride, .size = element_size});
    }

    /// True when the elements can be read in place as T: sizeof(T) matches, and both the start pointer and
    /// the stride are aligned for T.
    /// glTF only guarantees component alignment relative to the buffer start, and a caller's pin can sit at
    /// any address, so this can legitimately be false — always check before as_strided.
    template <class T>
    [[nodiscard]] bool is_typed_as() const
    {
        if (i64(sizeof(T)) != element_size)
            return false;
        // isize is exactly pointer-wide here — all shaped-core targets are 64-bit.
        auto const address = reinterpret_cast<isize>(bytes.data());
        if (address % isize(alignof(T)) != 0)
            return false;
        return stride % i64(alignof(T)) == 0;
    }

    /// The elements as a strided view of T; non-owning, tied to this view's lifetime.
    /// Precondition: is_typed_as<T>().
    template <class T>
    [[nodiscard]] cc::strided_span<T const> as_strided() const
    {
        CC_ASSERT(is_typed_as<T>(), "accessor elements are not readable in place as T");
        return cc::strided_span<T const>(reinterpret_cast<T const*>(bytes.data()), count, stride);
    }

    /// Copy the elements out as tightly-packed T — the always-safe path: it works misaligned and
    /// de-interleaves a strided bufferView. No component conversion happens, so sizeof(T) must equal
    /// element_size.
    template <class T>
    [[nodiscard]] cc::result<cc::vector<T>> read_elements() const
    {
        static_assert(std::is_trivially_copyable_v<T>, "read_elements needs a trivially-copyable T");

        if (i64(sizeof(T)) != element_size)
            return cc::error("gltf: accessor element size does not match the requested type");

        auto out = cc::vector<T>();
        out.resize_to_uninitialized(count);
        for (auto i = i64(0); i < count; ++i)
            cc::memcpy(&out[isize(i)], bytes.data() + i * stride, sizeof(T));
        return cc::move(out);
    }
};

// import issues
// -------------------------------------------------------------------------------------------------

/// Why the reader recorded an issue.
/// None of these fails the read — a condition that would is a `cc::result` error instead.
enum class issue_kind : u8
{
    /// The file uses a feature this reader does not implement, and it was skipped.
    /// Nothing is wrong with the file; the gap is ours.
    unsupported,
    /// A reference the reader could not follow: an external URI with no read_options::resolve_uri,
    /// or an image whose backing buffer stayed unresolved. The data is simply absent.
    unresolved,
    /// The file violates the spec in a way the reader chose to tolerate — an unknown enumerant, a
    /// half-stated bound, sloppy GLB padding. The named property fell back to its default.
    malformed,
};

/// One thing the reader noticed and did not fail on.
/// The message names the offending element by index, so it can be shown to a user as-is.
struct issue
{
    issue_kind kind = issue_kind::unsupported;
    cc::string message;
};

// the document
// -------------------------------------------------------------------------------------------------

/// The faithful parse of a glTF 2.0 document. Read-once; every vector mirrors the file's array order.
/// The `*_of` helpers resolve the flattened runs, and `find` resolves a typed index (nullptr for `invalid`).
struct data
{
    container source = container::gltf;
    asset_info asset;

    /// Everything the reader skipped, could not resolve, or tolerated, in the order it was noticed.
    /// A successful read with a non-empty `issues` is the normal case for a real-world asset — check it
    /// before concluding that what you got back is everything the file described.
    cc::vector<issue> issues;

    /// `extensionsUsed` / `extensionsRequired`, recorded verbatim.
    /// A non-empty extensions_required never reaches a caller: read fails on it, because the spec says a
    /// client that cannot support a required extension must refuse the file.
    cc::vector<cc::string> extensions_used;
    cc::vector<cc::string> extensions_required;

    cc::vector<buffer> buffers;
    cc::vector<buffer_view> buffer_views;
    cc::vector<accessor> accessors;
    cc::vector<f32> accessor_bounds; // the min / max arena the accessors point into

    cc::vector<attribute> attributes; // every primitive attribute, flattened
    cc::vector<primitive> primitives; // every mesh primitive, flattened
    cc::vector<mesh> meshes;

    cc::vector<node_index> node_children; // every node's children, flattened
    cc::vector<node> nodes;

    cc::vector<node_index> scene_nodes; // every scene's root nodes, flattened
    cc::vector<scene> scenes;
    scene_index default_scene = scene_index::invalid; // the document's `scene`

    cc::vector<material> materials;
    cc::vector<texture> textures;
    cc::vector<image> images;
    cc::vector<sampler> samplers;

    // import issues
public:
    [[nodiscard]] bool has_issues() const { return !issues.empty(); }

    /// Whether any recorded issue is of that kind — "did we skip a feature?", "is anything unresolved?".
    [[nodiscard]] bool has_issue_of(issue_kind kind) const;

    /// Every recorded issue joined into one newline-separated block, empty when there are none.
    /// For logging the import in one go; walk `issues` when the caller needs to act per issue.
    [[nodiscard]] cc::string issue_report() const;

    // index lookup
public:
    /// The element a typed index designates, or nullptr for `invalid`.
    /// read validates every index it stores, so nullptr always means "the file left this property out".
    [[nodiscard]] buffer const* find(buffer_index i) const { return impl_find(buffers, i); }
    [[nodiscard]] buffer_view const* find(buffer_view_index i) const { return impl_find(buffer_views, i); }
    [[nodiscard]] accessor const* find(accessor_index i) const { return impl_find(accessors, i); }
    [[nodiscard]] mesh const* find(mesh_index i) const { return impl_find(meshes, i); }
    [[nodiscard]] node const* find(node_index i) const { return impl_find(nodes, i); }
    [[nodiscard]] scene const* find(scene_index i) const { return impl_find(scenes, i); }
    [[nodiscard]] material const* find(material_index i) const { return impl_find(materials, i); }
    [[nodiscard]] texture const* find(texture_index i) const { return impl_find(textures, i); }
    [[nodiscard]] image const* find(image_index i) const { return impl_find(images, i); }
    [[nodiscard]] sampler const* find(sampler_index i) const { return impl_find(samplers, i); }

    // flattened runs
public:
    [[nodiscard]] cc::span<primitive const> primitives_of(mesh const& m) const
    {
        return cc::span<primitive const>(primitives)
            .subspan({.offset = isize(m.first_primitive), .size = isize(m.primitive_count)});
    }
    [[nodiscard]] cc::span<attribute const> attributes_of(primitive const& p) const
    {
        return cc::span<attribute const>(attributes)
            .subspan({.offset = isize(p.first_attribute), .size = isize(p.attribute_count)});
    }
    [[nodiscard]] cc::span<node_index const> children_of(node const& n) const
    {
        return cc::span<node_index const>(node_children)
            .subspan({.offset = isize(n.first_child), .size = isize(n.child_count)});
    }
    [[nodiscard]] cc::span<node_index const> nodes_of(scene const& s) const
    {
        return cc::span<node_index const>(scene_nodes).subspan({.offset = isize(s.first_node), .size = isize(s.node_count)});
    }

    /// The accessor behind a named attribute semantic; `invalid` when the primitive has no such attribute.
    [[nodiscard]] accessor_index find_attribute(primitive const& p, cc::string_view semantic) const;

    /// The accessor's stated `min` / `max`; empty when the file did not state them.
    [[nodiscard]] cc::span<f32 const> min_of(accessor const& a) const
    {
        return cc::span<f32 const>(accessor_bounds)
            .subspan({.offset = isize(a.first_bound), .size = isize(a.bound_count) / 2});
    }
    [[nodiscard]] cc::span<f32 const> max_of(accessor const& a) const
    {
        auto const half = isize(a.bound_count) / 2;
        return cc::span<f32 const>(accessor_bounds).subspan({.offset = isize(a.first_bound) + half, .size = half});
    }

    // accessor data
public:
    /// Resolve an accessor to its element bytes.
    /// Fails when the accessor has no bufferView (the spec's implicit all-zero data, which this reader does
    /// not synthesize) or when the underlying buffer was never resolved.
    [[nodiscard]] cc::result<accessor_view> view_of(accessor const& a) const;
    [[nodiscard]] cc::result<accessor_view> view_of(accessor_index i) const;

    /// The primitive's index buffer widened to u32, accepting u8 / u16 / u32 scalar accessors.
    /// Fails when the primitive is non-indexed — check `p.indices != accessor_index::invalid` first.
    [[nodiscard]] cc::result<cc::vector<u32>> read_indices(primitive const& p) const;

    // implementation
private:
    template <class Element, class Index>
    [[nodiscard]] static Element const* impl_find(cc::vector<Element> const& elements, Index i)
    {
        auto const raw = isize(int(i));
        return raw >= 0 && raw < elements.size() ? &elements[raw] : nullptr;
    }
};

// reading
// -------------------------------------------------------------------------------------------------

/// Which container `bytes` is in.
/// Never fails: anything that does not open with the GLB magic is treated as a JSON document, so a malformed
/// file reports a JSON parse error with a byte offset instead of a useless "unrecognized container".
[[nodiscard]] container detect_container(cc::span<byte const> bytes);

/// Reader knobs. Every default means "touch nothing outside the input bytes".
struct read_options
{
    /// Called once per buffer / image carrying an external (non-`data:`) URI, in declaration order.
    /// Return the referenced file's bytes to resolve it, or an error to fail the whole read.
    /// Unset (the default): external URIs stay unresolved — the `uri` is recorded and `data` stays empty.
    /// The URI arrives exactly as written: percent-decoding and joining against a base path are the
    /// resolver's job, because babel owns no filesystem policy.
    cc::function_ref<cc::result<cc::pinned_data<byte const>>(cc::string_view uri)> resolve_uri;
};

/// Parse a .gltf or .glb, auto-detecting the container.
/// This is the zero-copy entry point: every embedded buffer and every bufferView-backed image comes back as
/// a subview of `bytes` sharing its owner, so nothing bulk is copied and the views outlive `bytes` itself.
[[nodiscard]] cc::result<data> read(cc::pinned_data<byte const> bytes, read_options opts = {});

/// Convenience for a mutable pin; forwards to the immutable overload.
[[nodiscard]] cc::result<data> read(cc::pinned_data<byte> const& bytes, read_options opts = {});

/// Convenience: slurps the stream, then pins the slurped buffer (moved into the pin, not copied).
[[nodiscard]] cc::result<data> read(cc::read_stream& in, read_options opts = {});

/// Convenience: COPIES the input into an owned pin first, so the returned buffers stay valid independently
/// of the caller's memory. Pass a cc::pinned_data to avoid that copy.
[[nodiscard]] cc::result<data> read(cc::span<byte const> bytes, read_options opts = {});
[[nodiscard]] cc::result<data> read(cc::string_view text, read_options opts = {});
} // namespace babel::gltf
