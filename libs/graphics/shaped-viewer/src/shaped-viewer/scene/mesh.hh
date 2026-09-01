#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/mesh_flags.hh>
#include <shaped-viewer/scene/mesh_texture.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/transform/transform.hh>

/// What this mesh has been turned into, the last time it was placed.
///
/// It is a CACHE and never an identity: every payload is content-hashed, so placing a mesh against a manager that has never
/// seen it produces exactly the ids the slot would have held.
/// What the slot buys is that a repeat placement is a pointer compare instead of a hash lookup per payload, and that
/// `mesh::is_ready` is answerable from the mesh alone.
struct sv::impl::mesh_gpu_slot
{
    /// Identity ONLY, and never dereferenced.
    ///
    /// A mesh may outlive the manager it was placed against — it owns its bytes and needs no device to exist — so a stale
    /// pointer here is possible.
    /// Comparing one is sound where following one is not, which is why nothing ever does the latter.
    void const* manager = nullptr;

    /// The resources minted for this mesh against that manager.
    sv::resident_mesh resources;

    /// Whether those resources had all reached the GPU, as of that placement.
    bool ready = false;
};

/// One renderable mesh: geometry placed by a transform, drawn by a material, with everything that material may need
/// alongside it — and whatever GPU resources it has been given so far.
///
/// **This is the mesh a caller builds and holds.**
/// It needs no device to exist: every payload is pinned and content-hashed, so a mesh can be built, processed and passed
/// around long before a viewer does.
/// Placing it is what turns those payloads into resources, and `is_ready` is how it says whether they have arrived —
/// a mesh whose geometry is still uploading draws as a placeholder rather than not at all.
///
/// `sv::resident_mesh` is the same mesh once it is nothing but resources.
/// That one is the renderer's, and a caller rarely names it; see libs/graphics/shaped-viewer/docs/asset-loading.md.
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

    /// The object-space extent, when something already knew it — glTF states one per accessor, so an import never has
    /// to scan the positions to find it.
    ///
    /// Empty means "nobody said", and `create_mesh` then computes it from the positions.
    /// It is here rather than only on `sv::resident_mesh` because a box obtained without touching a payload byte is exactly what
    /// a placeholder needs while the geometry is still arriving.
    cc::optional<tg::aabb3f> bounds;

    /// What placing this mesh produced — see `impl::mesh_gpu_slot`.
    ///
    /// Mutable because placing a mesh READS it: `scene_ref::add_mesh` takes a `mesh const&`, and an asset hands out
    /// `mesh const&`, while the whole point of the slot is to remember what that placement produced.
    /// Not thread-safe, like the rest of the authoring API — a mesh is placed from the thread that owns the frame.
    mutable impl::mesh_gpu_slot cache;

    [[nodiscard]] bool is_visible() const { return flags.has(mesh_flag::visible); }

    /// Whether this mesh's resources had all reached the GPU, as of the last time it was placed.
    ///
    /// False for a mesh nobody has placed yet, which is the honest answer rather than a special case: nothing has been
    /// asked to upload it, so nothing has.
    /// It is a snapshot rather than a live query — placing the mesh again is what refreshes it, which is exactly the
    /// cadence a frame loop already runs at.
    [[nodiscard]] bool is_ready() const { return cache.ready; }
};
