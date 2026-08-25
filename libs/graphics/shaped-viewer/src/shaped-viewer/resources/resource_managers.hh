#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/span.hh> // cc::span
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format / attribute_frequency, which a record carries
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
/// `PrimitiveIndex()` in `sv::pbr_raytrace_routine`'s closest-hit.
/// The path tracer reads none of this — a material there is a `sv::material` resolved into a per-instance parameter block.
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

private:
    explicit material_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};

/// One uploaded mesh attribute: its bytes as a byte-address buffer, plus what a shader has to know to read them.
///
/// Elements are tightly packed from offset 0, so the descriptor a parameter block carries is this epoch's bindless index of
/// `data`, offset 0, and `format.size_bytes()` as the stride.
struct sv::attribute_record
{
    sg::buffer<byte> data;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);
    attribute_frequency frequency = attribute_frequency::per_vertex;
    isize element_count = 0;
};

/// Hands out `attribute_id`s for arbitrary mesh attributes, with LRU budgeting (see resource_budget).
///
/// This is what makes the `mesh_attribute` rank of the material chain reach the GPU at all: any attribute a material resolves to,
/// at any format and frequency, rather than the four PBR names the old repack knew.
/// Keyed on the attribute's own `hash`, so a mesh re-acquired every frame costs one lookup.
class sv::attribute_manager : public impl::lru_pool<attribute_id, attribute_record>
{
public:
    /// A manager that uploads into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static attribute_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The attribute_id for `attribute.hash`, resident from a prior acquire (O(1)), or a freshly uploaded one.
    /// The bytes are uploaded on one command list submitted before returning, so the id resolves immediately.
    [[nodiscard]] attribute_id acquire(mesh_attribute const& attribute);

private:
    explicit attribute_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};

/// How much of a resource has actually reached the GPU.
///
/// An id is handed out at once and never blocks, so a caller always has something to draw — what varies is how
/// good it is yet.
/// `resolve` therefore answers with a level as well as a resource, and a renderer decides for itself whether to
/// draw the placeholder, the base level, or wait.
enum class sv::residency : sv::u8
{
    pending,       ///< nothing on the GPU yet; only a placeholder can be drawn
    base_resident, ///< the base level is up and sampling works, at reduced quality
    complete,      ///< everything the policy asked for is up
};

/// One uploaded texture and how much of it has landed.
///
/// The whole chain is allocated on the first acquire even when only the base level is supplied, so generating the rest later
/// fills this texture in place rather than replacing it.
struct sv::texture_record
{
    sg::texture_2d texture;

    residency state = residency::pending;

    /// How many mip levels the pixels carried, and how many the texture has room for.
    i32 uploaded_mips = 0;
    i32 total_mips = 1;
};

/// Hands out `texture_id`s and owns the texture behind each, with LRU budgeting (see resource_budget).
///
/// It owns GPU resources and nothing else.
/// The bindless element a shader reaches a texture through is acquired per epoch by `gpu_resource_manager`, which is the one
/// thing that can also record it into that epoch's access declaration.
class sv::texture_manager : public impl::lru_pool<texture_id, texture_record>
{
public:
    /// A manager that uploads into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static texture_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The texture_id for `texture.hash`, resident from a prior acquire (O(1)), or a freshly uploaded one.
    ///
    /// On a miss the texture is created and every mip the data carries is uploaded on one command list submitted
    /// before returning, so the id resolves immediately.
    /// A texture whose data carried fewer mips than the shape allows comes back `base_resident`, and the
    /// follow-up that fills the rest is the gpu_resource_manager's to schedule.
    [[nodiscard]] texture_id acquire(texture_data const& texture);

    /// Marks `id`'s chain filled — what the manager calls once it has recorded the mip generation.
    /// A no-op for an id that has been evicted since.
    void mark_mips_complete(texture_id id);

private:
    explicit texture_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;
};
