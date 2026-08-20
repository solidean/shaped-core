#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh> // cc::span
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <typed-geometry/linalg/pos.hh>

/// How much a resource manager may keep resident, and how long an unused resource lingers.
///
/// Defaults are unbounded — a manager keeps everything until dropped.
/// Set a `max_bytes` to cap GPU memory, which evicts least-recently-used resources when over it.
/// Set a `max_idle_epochs` to reclaim resources no view has touched for that many frames.
/// The budget must be large enough for a frame's working set; an id whose resource has been evicted resolves to null.
struct sv::resource_budget
{
    isize max_bytes = 0;        ///< 0 => unbounded
    isize max_idle_epochs = -1; ///< < 0 => never idle-evict; 0 => evict once unused for a frame
};

/// Configuration for a single resource manager.
/// Just its LRU budget for now — more knobs land here.
struct sv::manager_config
{
    resource_budget budget = {};
};

/// One uploaded mesh: its geometry buffers and the BLAS built from them.
///
/// The BLAS is built once, when the mesh is acquired — a scene item then just references the mesh, and the
/// renderer rebuilds only the (cheap) TLAS each frame.
///
/// Indexed and non-indexed geometry stay distinct all the way down.
/// An `indexed_triangle_data` acquire uploads the caller's index buffer and builds an indexed BLAS, while a `triangle_data` acquire uploads nothing extra and builds a non-indexed one.
/// `is_indexed` is what a shader branches on, reaching the closest-hit through the frame constants.
/// It is also the only thing that makes `indices` meaningful.
struct sv::mesh_record
{
    sg::buffer<tg::pos3f> vertices;

    /// The mesh's own indices when `is_indexed`; otherwise the manager's stand-in buffer, which no shader
    /// reads but every binding group must still cover.
    sg::buffer<u32> indices;
    bool is_indexed = false;

    isize triangle_count = 0;
    sg::blas_handle blas;
};

/// Hands out `mesh_id`s and owns the geometry + BLAS behind each, with LRU budgeting (see resource_budget).
/// This is where the one-time BLAS build lives (a BLAS build is setup, not a per-frame pass, so it stays
/// here rather than in a routine).
class sv::mesh_manager : public impl::lru_pool<mesh_id, mesh_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static mesh_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The mesh_id for `mesh.hash`, resident from a prior acquire (O(1)), or a freshly uploaded one.
    /// On a miss the geometry is uploaded and BLAS-built on command lists submitted before returning, so the id is usable immediately.
    /// Ray tracing must be supported on the context.
    ///
    /// Non-indexed: `positions` is a triangle list, 3 vertices per triangle, count a multiple of 3.
    [[nodiscard]] mesh_id acquire(triangle_data const& mesh);
    /// Indexed: `indices` names triangles into `positions`, count a multiple of 3. Nothing is de-indexed —
    /// the BLAS is built from the index buffer and the closest-hit reads through it.
    [[nodiscard]] mesh_id acquire(indexed_triangle_data const& mesh);

private:
    explicit mesh_manager(sg::context& ctx) : _ctx(ctx) {}

    /// The stand-in a non-indexed record binds as `Indices`, created on first use and recorded onto `cmd`.
    /// Its contents are never read — it exists only so the trace's binding group is complete.
    [[nodiscard]] sg::buffer<u32> _acquire_index_stand_in(sg::command_list& cmd);

    sg::context& _ctx;
    sg::buffer<u32> _index_stand_in;
};

/// One uploaded material set: a StructuredBuffer of `pbr_material_gpu`, one entry per triangle, indexed by
/// `PrimitiveIndex()` in the closest-hit.
struct sv::material_record
{
    sg::buffer<pbr_material_gpu> materials;
    isize count = 0;
};

/// Hands out `material_set_id`s and owns the per-set material buffer, with LRU budgeting (see resource_budget).
class sv::material_manager : public impl::lru_pool<material_set_id, material_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static material_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The material_set_id for `materials.hash`, resident from a prior acquire (O(1)), or a freshly uploaded one.
    /// On a miss the set is packed to its GPU layout and uploaded into a read-only structured buffer on one command list submitted before returning.
    [[nodiscard]] material_set_id acquire(material_data const& materials);

    /// The same, for per-face PBR carried as the `sv::pbr_attribute` attributes of a mesh.
    ///
    /// The cache key is folded from the attributes' own content hashes rather than recomputed over their bytes, so
    /// re-acquiring an unchanged mesh every frame stays O(1) — which is what lets `scene_ref::add_mesh` do this for
    /// the caller.
    /// An attribute that is absent contributes `pbr_material`'s default for its field; one that is present must be
    /// `per_triangle`, hold the element type its name documents, and carry `triangle_count` elements.
    [[nodiscard]] material_set_id acquire(cc::span<mesh_attribute const> attributes, isize triangle_count);

private:
    explicit material_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};

/// Placeholder texture manager — the seam for material textures once the scene grows past flat per-triangle PBR.
/// Empty this slice; it exists so `texture_id` has an owner to point at.
class sv::texture_manager
{
public:
    /// Takes the context to match the other managers; unused this slice (no config yet).
    [[nodiscard]] static texture_manager create(sg::context&) { return texture_manager(); }

    [[nodiscard]] bool contains(texture_id) const { return false; }
    [[nodiscard]] isize count() const { return 0; }
};

/// Per-manager configuration for a whole scene's resources.
struct sv::scene_resources_config
{
    manager_config meshes = {};
    manager_config materials = {};
};

/// The bundle of resource managers a `viewer_definition` resolves its ids against.
/// One per scene; passed to `sv::view_renderer::execute` / `sv::viewer_renderer::execute`.
/// All three managers share the context it is created with, which must outlive it.
///
/// Set per-manager budgets through the config (`scene_resources::create(ctx, {.meshes = {.budget = ...}})`).
/// `begin_frame` is the caller's to run, once per frame, before the first view resolves its ids — no routine does it, because none of them knows how many more views the frame holds.
class sv::scene_resources
{
public:
    /// Creates all three managers on `ctx`, each budgeted by its slice of `cfg`.
    [[nodiscard]] static scene_resources create(sg::context& ctx, scene_resources_config const& cfg = {});

    /// Advance every budgeted manager to epoch `e`, running its idle + budget eviction first.
    void begin_frame(sg::epoch e)
    {
        meshes.begin_frame(e);
        materials.begin_frame(e);
    }

    mesh_manager meshes;
    material_manager materials;
    texture_manager textures;

private:
    scene_resources(mesh_manager meshes, material_manager materials, texture_manager textures)
      : meshes(cc::move(meshes)), materials(cc::move(materials)), textures(cc::move(textures))
    {
    }
};
