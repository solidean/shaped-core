#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/mesh_flags.hh>
#include <shaped-viewer/scene/mesh_texture.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/transform/transform.hh>

/// One renderable mesh described entirely by CPU bytes: geometry placed by a transform, drawn by a material, with everything
/// that material may need alongside it.
///
/// This is the CPU half of the pair `sv::mesh` completes — see libs/graphics/shaped-viewer/docs/asset-loading.md.
/// Nothing here needs a device: every payload is pinned and content-hashed, so a mesh can be built, processed and passed around
/// long before a viewer exists, and a manager turns it into an `sv::mesh` with a lookup per payload.
/// Because the bytes are retained, an evicted GPU copy is a re-upload rather than a correctness problem.
///
/// The split inside it is deliberate.
/// `geometry` is the surface itself and nothing else — positions and, when indexed, the triangles naming them.
/// Everything a shader might want *on top of* that surface travels in the three open-ended lists rather than as fixed fields:
/// `attributes` per element of the geometry or per instance, `textures` per slot, and `flags` for the on/off decisions.
///
/// `material` is a thin id: material definitions live outside the mesh and are shared across many.
/// The material is what gives the lists their meaning — it decides which attribute names it samples, which texture slots it binds, and how the flags change what it emits.
/// So a mesh carries data a material may not use, and misses data another material would want; neither is an error, and a material falls back rather than failing.
///
/// A mesh_data is a value: copying shares the pinned payloads (a refcount bump, not a deep copy of the buffers), so passing one around is cheap.
struct sv::mesh_data
{
    /// human-readable, for debugging and for picking a mesh out of a scene; not an identity — nothing dedupes on it
    cc::string name;

    sv::triangle_geometry geometry;

    /// arbitrary extra data, looked up by name by the material — per element of the geometry, or one value for the whole mesh at `per_instance`
    cc::vector<mesh_attribute> attributes;

    /// placement in world space; may scale or shear, so build it from tg's factories and `tg::compose`
    tg::affine_transform3f transform = {};

    /// how this mesh is rendered — the definition lives elsewhere and is shared
    material_id material = material_id::invalid;

    mesh_flags flags = mesh_flags_default;

    /// textures the material may bind, by slot name
    cc::vector<mesh_texture_data> textures;

    /// The object-space extent, when something already knew it — glTF states one per accessor, so an import never has
    /// to scan the positions to find it.
    ///
    /// Empty means "nobody said", and `create_mesh` then computes it from the positions.
    /// It is here rather than only on `sv::mesh` because a box obtained without touching a payload byte is exactly what
    /// a placeholder needs while the geometry is still arriving.
    cc::optional<tg::aabb3f> bounds;

    [[nodiscard]] bool is_visible() const { return flags.has(mesh_flag::visible); }
};
