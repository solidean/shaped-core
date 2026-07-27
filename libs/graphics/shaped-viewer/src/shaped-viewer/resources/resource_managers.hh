#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <typed-geometry/linalg/pos.hh>

namespace sv
{
/// How much a resource manager may keep resident, and how long an unused resource lingers.
///
/// Defaults are unbounded — a manager keeps everything until dropped. Set a `max_bytes` to cap GPU memory
/// (least-recently-used resources are evicted when over it), and/or a `max_idle_epochs` to reclaim resources
/// no view has touched for that many frames. The budget must be large enough for a frame's working set; an
/// id whose resource has been evicted resolves to null.
struct resource_budget
{
    isize max_bytes = 0;        ///< 0 => unbounded
    isize max_idle_epochs = -1; ///< < 0 => never idle-evict; 0 => evict once unused for a frame
};

/// Configuration for a single resource manager. Just its LRU budget for now — more knobs land here.
struct manager_config
{
    resource_budget budget = {};
};

/// One uploaded mesh: its vertex buffer and the BLAS built from it.
///
/// The BLAS is built once, when the mesh is acquired — a scene item then just references the mesh, and the
/// renderer rebuilds only the (cheap) TLAS each frame. Non-indexed triangle list: `triangle_count == vertices / 3`.
struct mesh_record
{
    sg::buffer<tg::pos3f> vertices;
    isize triangle_count = 0;
    sg::blas_handle blas;
};

/// Hands out `mesh_id`s and owns the geometry + BLAS behind each, with LRU budgeting (see resource_budget).
/// This is where the one-time BLAS build lives (a BLAS build is setup, not a per-frame pass, so it stays
/// here rather than in a routine).
class mesh_manager : public impl::lru_pool<mesh_id, mesh_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static mesh_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// Uploads `positions` (a non-indexed triangle list — 3 vertices per triangle, count a multiple of 3)
    /// and builds its BLAS, all on one command list submitted before returning. Ray tracing must be
    /// supported on the context.
    [[nodiscard]] mesh_id acquire(cc::span<tg::pos3f const> positions);

private:
    explicit mesh_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};

/// One uploaded material set: a StructuredBuffer of `pbr_material_gpu`, one entry per triangle, indexed by
/// `PrimitiveIndex()` in the closest-hit.
struct material_record
{
    sg::buffer<pbr_material_gpu> materials;
    isize count = 0;
};

/// Hands out `material_set_id`s and owns the per-set material buffer, with LRU budgeting (see resource_budget).
class material_manager : public impl::lru_pool<material_set_id, material_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static material_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// Uploads `materials` into a read-only structured buffer on one command list submitted before returning.
    [[nodiscard]] material_set_id acquire(cc::span<pbr_material const> materials);

private:
    explicit material_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};

/// Placeholder texture manager — the seam for material textures once the scene grows past flat per-triangle
/// PBR. Empty this slice; it exists so `texture_id` has an owner to point at.
class texture_manager
{
public:
    /// Takes the context to match the other managers; unused this slice (no config yet).
    [[nodiscard]] static texture_manager create(sg::context&) { return texture_manager(); }

    [[nodiscard]] bool contains(texture_id) const { return false; }
    [[nodiscard]] isize count() const { return 0; }
};

/// Per-manager configuration for a whole scene's resources.
struct scene_resources_config
{
    manager_config meshes = {};
    manager_config materials = {};
};

/// The bundle of resource managers a `viewer_definition` resolves its ids against. One per scene; passed to
/// `sv::view_renderer::execute`. All three managers share the context it is created with, which must outlive it.
///
/// Set per-manager budgets through the config (`scene_resources::create(ctx, {.meshes = {.budget = ...}})`).
/// The view_renderer calls `begin_frame` for you each frame; call it yourself only if you drive resources
/// outside a render.
class scene_resources
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
} // namespace sv
