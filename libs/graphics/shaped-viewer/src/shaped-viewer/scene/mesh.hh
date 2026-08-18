#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/mesh_flags.hh>
#include <shaped-viewer/scene/mesh_texture.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/transform/transform.hh>

/// One renderable mesh: geometry placed by a transform, drawn by a material, with everything that material may need alongside it.
///
/// The split is deliberate.
/// `geometry` is the surface itself and nothing else — positions and, when indexed, the triangles naming them.
/// Everything a shader might want *on top of* that surface travels in the three open-ended lists rather than as fixed fields:
/// `attributes` per element of the geometry or per instance, `textures` per slot, and `flags` for the on/off decisions.
///
/// `material` is a thin id: material definitions live outside the mesh and are shared across many.
/// The material is what gives the lists their meaning — it decides which attribute names it samples, which texture slots it binds, and how the flags change what it emits.
/// So a mesh carries data a material may not use, and misses data another material would want; neither is an error, and a material falls back rather than failing.
///
/// A mesh is a value: copying shares the pinned payloads (a refcount bump, not a deep copy of the buffers), so passing one around is cheap.
struct sv::mesh
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
    cc::vector<mesh_texture> textures;

    [[nodiscard]] bool is_visible() const { return flags.has(mesh_flag::visible); }
};
