#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh> // cc::span
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format / attribute_frequency, which a record carries
#include <shaped-viewer/scene/pbr_material.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
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

/// How much of a resource has actually reached the GPU.
///
/// An id is handed out at once and never blocks, so a caller always has something to draw — what varies is how
/// good it is yet.
/// A renderer decides for itself whether to draw the placeholder, the base level, or wait.
///
/// **Every resource kind carries one**, not just textures: it is the state half of the record model in
/// libs/graphics/shaped-viewer/docs/asset-loading.md, and one concept across the kinds rather than a parallel one per
/// kind is what lets the substitution paths be written once.
/// `base_resident` is the one level only a texture can be at — a mesh or an attribute is either up or it is not.
enum class sv::residency : sv::u8
{
    pending,       ///< nothing on the GPU yet; only a placeholder can be drawn
    base_resident, ///< the base level is up and sampling works, at reduced quality
    complete,      ///< everything the policy asked for is up

    /// Producing it failed and retrying it will not help until something changes.
    ///
    /// Distinct from `pending`, which a caller is right to keep waiting on: a failed resource draws its placeholder
    /// forever, and whatever noticed says so once rather than every frame.
    failed,
};

/// One uploaded mesh: its geometry buffers and the BLAS built from them.
///
/// The BLAS is built once, when the mesh is acquired — a scene item then just references the mesh, and the
/// renderer rebuilds only the (cheap) TLAS each frame.
///
/// Indexed and non-indexed geometry stay distinct all the way down.
/// An `indexed_triangle_data` acquire uploads the caller's index buffer and builds an indexed BLAS, while a `triangle_data` acquire uploads nothing extra and builds a non-indexed one.
/// `is_indexed` is what a shader branches on.
/// It reaches the path tracer's closest-hit per instance, through `instance_gpu::is_indexed` and `InstanceID()`;
/// the flat `pbr_raytrace_routine` still takes it per frame, in `frame_constants_gpu::mesh_is_indexed`.
/// It is also the only thing that makes `indices` meaningful.
struct sv::mesh_record
{
    /// How much of this mesh has reached the GPU: `complete` once its buffers are uploaded and its BLAS is built.
    /// While `pending` there is no BLAS to trace, so a placeholder stands in for it — see `mesh_manager::placeholder_blas`.
    residency state = residency::pending;

    sg::buffer<tg::pos3f> vertices;

    /// The mesh's own indices when `is_indexed`; otherwise the manager's stand-in buffer, which no shader
    /// reads but every binding group must still cover.
    sg::buffer<u32> indices;
    bool is_indexed = false;

    isize triangle_count = 0;
    sg::blas_handle blas;

    /// The object-space box, when the payload declared one — the summary half of the record.
    ///
    /// It outlives the payload, which is the point: a pending mesh is drawn as the shared placeholder box scaled onto
    /// this, and a mesh whose bounds nobody stated is skipped instead, since guessing an extent would place it wrong.
    cc::optional<tg::aabb3f> bounds;
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

    /// The unit cube every pending mesh is drawn as, built on first use.
    ///
    /// ONE BLAS for every placeholder in a scene: it spans `[0,1]^3`, and the extent a particular mesh should occupy
    /// is folded into its TLAS transform instead — so a hundred meshes still arriving cost one acceleration structure
    /// and a hundred 3x4 matrices.
    [[nodiscard]] sg::blas_handle const& placeholder_blas();

    /// The cube's own vertices, which an instance record must name alongside its BLAS.
    ///
    /// A hit reads positions back out of the instance to recompute the geometric normal, so pointing at the real
    /// mesh's buffer while tracing the cube would shade it from a triangle it never hit.
    /// Built by the same first call `placeholder_blas` is.
    [[nodiscard]] sg::buffer<tg::pos3f> const& placeholder_vertices();

    /// The stand-in index buffer a non-indexed record binds; the placeholder is non-indexed, so it binds this too.
    [[nodiscard]] sg::buffer<u32> const& index_stand_in();

    /// Uploads queued geometry and builds its BLAS, oldest first, spending at most `max_bytes` (<= 0 = everything).
    ///
    /// Returns the bytes it spent, which is what a caller draining several managers against one budget subtracts.
    /// A request that does not fit is not split — half a vertex buffer would build a BLAS over a hole — so the first
    /// one always runs whatever its size, and the queue drains in at most one mesh per epoch in the worst case.
    isize record_uploads(isize max_bytes);

    /// How many meshes are still waiting to be uploaded.
    [[nodiscard]] isize pending_upload_count() const { return _pending.size(); }

private:
    explicit mesh_manager(sg::context& ctx) : _ctx(ctx) {}

    /// One queued geometry: the payload it was acquired from, held until the upload runs.
    ///
    /// The pins are what make deferral safe — the caller's `mesh_data` may be long gone by the time this drains, and
    /// nothing else keeps those bytes alive.
    struct pending_mesh
    {
        mesh_id id = mesh_id::invalid;
        cc::pinned_data<tg::pos3f const> positions;
        cc::pinned_data<u32 const> indices; ///< empty for a raw triangle list
        isize bytes = 0;
    };

    /// The stand-in a non-indexed record binds as `Indices`, created on first use and recorded onto `cmd`.
    /// Its contents are never read — it exists only so the trace's binding group is complete.
    [[nodiscard]] sg::buffer<u32> _acquire_index_stand_in(sg::command_list& cmd);

    sg::context& _ctx;
    sg::buffer<u32> _index_stand_in;
    sg::blas_handle _placeholder_blas;
    sg::buffer<tg::pos3f> _placeholder_vertices;
    cc::vector<pending_mesh> _pending;
};

/// One uploaded material set: a StructuredBuffer of `pbr_material_gpu`, one entry per triangle, indexed by
/// `PrimitiveIndex()` in `sv::pbr_raytrace_routine`'s closest-hit.
/// The path tracer reads none of this — a material there is a `sv::material` resolved into a per-instance parameter block.
struct sv::material_record
{
    residency state = residency::pending;

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
    /// The buffer exists from the moment the id is minted, so a descriptor pointing at it is always valid — what
    /// `pending` means here is that its CONTENTS have not landed, and a shader reading it early reads zeros.
    residency state = residency::pending;

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

    /// The attribute_id for `attribute.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// The BUFFER exists immediately, so a descriptor naming it is valid from here on; its contents land when
    /// `record_uploads` drains, and a shader that reads it before then reads zeros.
    /// That is why the manager's queue is drained ahead of the mesh one: an attribute is up before the geometry it
    /// belongs to is drawn with anything but a placeholder.
    [[nodiscard]] attribute_id acquire(mesh_attribute const& attribute);

    /// Uploads queued attribute bytes, oldest first, spending at most `max_bytes` (<= 0 = everything).
    isize record_uploads(isize max_bytes);

    [[nodiscard]] isize pending_upload_count() const { return _pending.size(); }

private:
    explicit attribute_manager(sg::context& ctx) : _ctx(ctx) {}

    struct pending_attribute
    {
        attribute_id id = attribute_id::invalid;
        cc::pinned_data<byte const> data;
    };

    sg::context& _ctx;
    cc::vector<pending_attribute> _pending;
};

/// One uploaded texture and how much of it has landed.
///
/// The whole chain is allocated on the first acquire even when only the base level is supplied, so generating the rest later
/// fills this texture in place rather than replacing it.
/// The budget is charged for that whole chain from the first acquire, since a record's byte size is fixed at insert.
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
