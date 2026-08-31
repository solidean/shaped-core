#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh> // cc::span
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
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

    /// The mesh_id for `mesh.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// On a miss the buffers are created and the geometry is handed to `ctx.stream`, which carries it on the copy
    /// queue at its own pace — so an acquire costs an allocation and a hash lookup rather than a transfer.
    /// The id is usable at once and the mesh is `pending` until `record_settled` builds its BLAS; until then it is
    /// traced as `placeholder_blas`.
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

    /// Builds the BLAS for every mesh whose geometry has finished streaming, recording onto `cmd`.
    ///
    /// The build is what makes a mesh `complete`, and it may only be recorded once the transfer is observed done —
    /// which is exactly the contract `sg::stream_upload_handle` states: a list touching a streamed extent must be
    /// SUBMITTED after completion is observed, and `cmd` is submitted by the caller after this returns.
    ///
    /// A transfer that settled without delivering — cancelled, or failed — leaves its mesh `failed` rather than
    /// pending, so nothing waits on it forever.
    /// Returns how many meshes it finished.
    isize record_settled(sg::command_list& cmd);

    /// Blocks until every queued transfer has landed, then finishes them all.
    ///
    /// For a caller with no frame loop to drain the queue — a test tracing what it just built, or a one-pass tool.
    /// The transfers are promoted first, so nothing depends on this call's own ordering.
    void wait_for_settled();

    /// How many meshes are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

private:
    explicit mesh_manager(sg::context& ctx) : _ctx(ctx) {}

    /// The transfers one queued mesh is waiting on.
    ///
    /// The payload is NOT held here: `ctx.stream` took the pin, so the bytes are owned by the transfer and released
    /// when it settles.
    /// Dropping this entry cancels whatever has not been sent, which is what makes evicting a still-streaming mesh
    /// free rather than something to remember.
    struct pending_mesh
    {
        sg::stream_upload_handle vertices;
        sg::stream_upload_handle indices; ///< invalid for a raw triangle list
        isize bytes = 0;
    };

    /// Finishes `id` if its transfers have settled; returns whether it did.
    [[nodiscard]] bool _try_settle(sg::command_list& cmd, mesh_id id, pending_mesh& p);

    /// The stand-in a non-indexed record binds as `Indices`, created on first use and recorded onto `cmd`.
    /// Its contents are never read — it exists only so the trace's binding group is complete.
    [[nodiscard]] sg::buffer<u32> _acquire_index_stand_in(sg::command_list& cmd);

    sg::context& _ctx;
    sg::buffer<u32> _index_stand_in;
    sg::blas_handle _placeholder_blas;
    sg::buffer<tg::pos3f> _placeholder_vertices;
    cc::map<mesh_id, pending_mesh> _settling;
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

    /// Marks every attribute whose bytes have finished streaming `complete`; returns how many.
    /// Nothing is recorded — an attribute is done the moment its transfer is, since nothing has to be built from it.
    isize collect_settled();

    /// Blocks until every queued transfer has landed, then collects them.
    void wait_for_settled();

    /// How many attributes are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

private:
    explicit attribute_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;

    /// The in-flight transfer per queued attribute; dropping one cancels it.
    cc::map<attribute_id, sg::stream_upload_handle> _settling;
};

/// One uploaded texture and how much of it has landed.
///
/// The whole chain is allocated on the first acquire even when only the base level is supplied, so generating the rest later
/// fills this texture in place rather than replacing it.
/// The budget is charged for that whole chain from the first acquire, since a record's byte size is fixed at insert.
struct sv::texture_record
{
    sg::texture_2d texture;

    /// `pending` until the supplied levels land; then `base_resident` or `complete`, per `uploaded_mips`.
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

    /// The texture_id for `texture.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// On a miss the texture is created and every mip the data carries is handed to `ctx.stream`, so the id resolves
    /// immediately while the pixels are still in flight.
    /// It is `pending` until `collect_settled` sees the transfers land, and a slot sampling it meanwhile reads a 1x1
    /// placeholder seeded from the material's own factor rather than an empty texture.
    [[nodiscard]] texture_id acquire(texture_data const& texture);

    /// Advances every texture whose pixels have landed, and reports the ids that just became sampleable.
    ///
    /// `newly_resident` is appended to rather than cleared, and is what the owning manager turns into queued mip
    /// generation — which cannot be decided at acquire, since nothing has arrived then.
    /// Returns how many textures it advanced.
    isize collect_settled(cc::vector<texture_id>& newly_resident);

    /// Blocks until every queued transfer has landed, then collects them.
    void wait_for_settled(cc::vector<texture_id>& newly_resident);

    /// How many textures are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

    /// Marks `id`'s chain filled — what the manager calls once it has recorded the mip generation.
    /// A no-op for an id that has been evicted since.
    void mark_mips_complete(texture_id id);

private:
    explicit texture_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;

    /// The in-flight transfers per queued texture, one per supplied mip; dropping them cancels the upload.
    cc::map<texture_id, cc::vector<sg::stream_upload_handle>> _settling;
};
