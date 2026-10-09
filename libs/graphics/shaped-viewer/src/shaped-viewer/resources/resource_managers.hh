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
#include <shaped-viewer/resources/quadric_data.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/value_summary.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format / attribute_frequency, which a record carries
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

namespace sv
{
/// How many triangles one block of an emitter's area CDF covers — `tracer.emitter_block_size` and shaders/emitter_cdf.sgl's `block_size`.
inline constexpr u32 emitter_cdf_block_size = 256;

/// How many blocks an area CDF over `triangle_count` triangles holds.
[[nodiscard]] constexpr u32 emitter_cdf_block_count(isize triangle_count)
{
    return u32((triangle_count + isize(emitter_cdf_block_size) - 1) / isize(emitter_cdf_block_size));
}
} // namespace sv

/// One uploaded mesh: its geometry buffers, and the BLAS built from them once something traces it.
///
/// **The BLAS is built on demand**, by the first trace that reaches the mesh through `mesh_manager::acquire_blas`.
/// A mesh only ever rasterized never gets one, which is what lets a raster layer draw on a device without ray tracing.
/// Once built it stays, so a trace then rebuilds only the (cheap) TLAS each frame.
///
/// Indexed and non-indexed geometry stay distinct all the way down.
/// An `indexed_triangle_data` acquire uploads the caller's index buffer and builds an indexed BLAS, while a `triangle_data` acquire uploads nothing extra and builds a non-indexed one.
/// `is_indexed` is what a shader branches on.
/// It reaches the path tracer's closest-hit per instance, through `shaders::tracer::instance_record::is_indexed` and the hit's instance id.
/// It is also the only thing that makes `indices` meaningful.
struct sv::mesh_record
{
    /// How much of this mesh has reached the GPU: `complete` once its buffers are uploaded, whether or not a BLAS was built.
    /// While `pending` there is nothing to draw, so a placeholder box stands in for it — see `mesh_manager::placeholder_vertices`.
    residency state = residency::pending;

    sg::buffer<tg::pos3f> vertices;

    /// The mesh's own indices when `is_indexed`; otherwise the manager's stand-in buffer, which no shader
    /// reads but every binding group must still cover.
    sg::buffer<u32> indices;
    bool is_indexed = false;

    isize triangle_count = 0;

    /// Null until a trace asks `mesh_manager::acquire_blas` for it.
    sg::blas_handle blas;

    /// The object-space surface area, summed over the triangles on the acquire that uploaded them.
    /// What an emissive instance's selection weight starts from, so it is known before any CDF is built.
    f32 surface_area = 0;

    /// The area CDF next-event estimation picks one of the mesh's triangles by, null until a trace asks `mesh_manager::acquire_emitter_cdf` for it.
    /// A mesh no emissive instance places never gets one.
    /// `triangle_count + emitter_cdf_block_count(triangle_count) + 1` floats, laid out as shaders/emitter_cdf.sgl says.
    sg::buffer<f32> emitter_cdf;

    /// The object-space box, when the payload declared one — the summary half of the record.
    ///
    /// It outlives the payload, which is the point: a pending mesh is drawn as the shared placeholder box scaled onto
    /// this, and a mesh whose bounds nobody stated is skipped instead, since guessing an extent would place it wrong.
    cc::optional<tg::aabb3f> bounds;
};

/// Hands out `mesh_id`s and owns the geometry + BLAS behind each, with LRU budgeting (see resource_budget).
/// This is where the one-time BLAS build lives (a BLAS build is setup, not a per-frame pass, so it stays
/// here rather than in a routine), and it runs only for a mesh something traces.
class sv::mesh_manager : public impl::lru_pool<mesh_id, mesh_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static mesh_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The mesh_id for `mesh.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// On a miss the buffers are created and the geometry is handed to `ctx.stream`, which carries it on the copy
    /// queue at its own pace — so an acquire costs an allocation and a hash lookup rather than a transfer.
    /// The id is usable at once and the mesh is `pending` until `collect_settled` sees its geometry land; until then it
    /// is drawn as the placeholder box.
    /// Ray tracing need not be supported: nothing here builds an acceleration structure.
    ///
    /// Non-indexed: `positions` is a triangle list, 3 vertices per triangle, count a multiple of 3.
    [[nodiscard]] mesh_id acquire(triangle_data const& mesh);
    /// Indexed: `indices` names triangles into `positions`, count a multiple of 3. Nothing is de-indexed —
    /// the BLAS is built from the index buffer and the closest-hit reads through it.
    [[nodiscard]] mesh_id acquire(indexed_triangle_data const& mesh);

    /// The unit cube every pending mesh is traced as, built on first use; ray tracing must be supported on the context.
    ///
    /// ONE BLAS for every placeholder in a scene: it spans `[0,1]^3`, and the extent a particular mesh should occupy
    /// is folded into its TLAS transform instead — so a hundred meshes still arriving cost one acceleration structure
    /// and a hundred 3x4 matrices.
    [[nodiscard]] sg::blas_handle const& placeholder_blas();

    /// The cube's own vertices, which an instance record must name in place of the mesh's, built on first use.
    ///
    /// A hit reads positions back out of the instance to recompute the geometric normal, and a raster draw pulls its
    /// corners from it, so pointing at the real mesh's buffer while drawing the cube would read triangles it never had.
    [[nodiscard]] sg::buffer<tg::pos3f> const& placeholder_vertices();

    /// The stand-in index buffer a non-indexed record binds; the placeholder is non-indexed, so it binds this too.
    [[nodiscard]] sg::buffer<u32> const& index_stand_in();

    /// Marks every mesh whose geometry has finished streaming `complete`; returns how many.
    ///
    /// Nothing is recorded: a mesh is drawable the moment its transfer is done, and its BLAS waits for a trace.
    /// A transfer that settled without delivering — cancelled, or failed — leaves its mesh `failed` rather than
    /// pending, so nothing waits on it forever.
    isize collect_settled();

    /// `id`'s BLAS, recorded onto `cmd` the first time a caller asks and held from then on.
    ///
    /// `id` must be `complete`, and ray tracing must be supported on the context.
    /// The build may only be recorded once the transfer is observed done, which `complete` is — the contract
    /// `sg::stream_upload_handle` states: a list touching a streamed extent must be SUBMITTED after completion is
    /// observed, and `cmd` is submitted by the caller after this returns.
    /// The first call charges the BLAS's size to the budget.
    [[nodiscard]] sg::blas_handle const& acquire_blas(sg::command_list& cmd, mesh_id id);

    /// `id`'s area CDF, built onto `cmd` from the mesh's own buffers the first time a caller asks and held from then on.
    ///
    /// Null while the two kernels that build it are still compiling, and for good once they failed to, which is logged once:
    /// an emissive instance of the mesh then lights nothing that frame, and its emission reaches the camera alone.
    /// `id` must be `complete`, for the reason `acquire_blas` gives; no ray tracing is needed.
    /// The first build charges the CDF's size to the budget.
    [[nodiscard]] sg::buffer<f32> const* acquire_emitter_cdf(sg::command_list& cmd, mesh_id id);

    /// False once the kernels behind `acquire_emitter_cdf` failed to build, after which no CDF ever will be.
    [[nodiscard]] bool can_build_emitter_cdfs() const { return !_cdf_failed; }

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
    [[nodiscard]] bool _try_settle(mesh_id id, pending_mesh& p);

    /// The stand-in a non-indexed record binds as `Indices`, created on first use and recorded onto `cmd`.
    /// Its contents are never read — it exists only so the trace's binding group is complete.
    [[nodiscard]] sg::buffer<u32> _acquire_index_stand_in(sg::command_list& cmd);

    /// Whether the two kernels of shaders/emitter_cdf.sgl are built, starting them on first use; polled, never waited on.
    [[nodiscard]] bool _emitter_cdf_kernels_ready();

    sg::context& _ctx;
    sg::async_compute_pipeline _cdf_blocks;
    sg::async_compute_pipeline _cdf_total;
    bool _cdf_failed = false;
    sg::buffer<u32> _index_stand_in;
    sg::blas_handle _placeholder_blas;
    sg::buffer<tg::pos3f> _placeholder_vertices;
    cc::map<mesh_id, pending_mesh> _settling;
};

/// One uploaded quadric batch: its two buffers, and the procedural BLAS built over them once something traces it.
///
/// Two buffers rather than one, because the hardware and the shader want different things.
/// `aabbs` is the acceleration structure's build input and nothing reads it afterwards — an intersection shader cannot reach the
/// boxes the BLAS was built from.
/// `primitives` is what the intersection shader reads by the primitive index, through the bindless table.
///
/// The BLAS is built once, by the first trace that reaches the batch, exactly as a mesh's is: from then on only the (cheap) TLAS is
/// rebuilt per frame.
struct sv::quadric_set_record
{
    /// `complete` once both buffers are uploaded, whether or not a BLAS was built.
    /// While `pending` there is nothing to trace, so the batch is drawn as the shared placeholder box scaled onto `bounds`.
    residency state = residency::pending;

    /// the intersection shader's own data, read by the primitive index
    sg::buffer<quadric_gpu> primitives;

    /// the BLAS build input, one box per primitive; its layout is `D3D12_RAYTRACING_AABB`, which `tg::aabb3f` already is
    sg::buffer<tg::aabb3f> aabbs;

    isize primitive_count = 0;

    /// Null until a trace asks `quadric_manager::acquire_blas` for it.
    sg::blas_handle blas;

    /// The extent in the batch's own frame, when the payload declared one — the summary half of the record.
    /// It outlives the payload, which is the point: a pending batch is drawn as the placeholder box scaled onto this, and one
    /// whose bounds nobody stated is skipped instead, since guessing an extent would place it wrong.
    cc::optional<tg::aabb3f> bounds;
};

/// Hands out `quadric_set_id`s and owns the two buffers + procedural BLAS behind each, with LRU budgeting.
///
/// The quadric counterpart of `mesh_manager`, and deliberately the same shape: acquire queues transfers and returns at once,
/// `collect_settled` sees them land, `acquire_blas` builds the acceleration structure behind them, and residency says how far
/// along a batch is.
/// What differs is only that the BLAS is built over AABBs rather than triangles, and that the payload has to be SPLIT on the way
/// out — the authored form carries each primitive's box beside it, and the two go to different buffers.
class sv::quadric_manager : public impl::lru_pool<quadric_set_id, quadric_set_record>
{
public:
    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static quadric_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The quadric_set_id for `set.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// On a miss the two buffers are created, the authored primitives are split into them, and both payloads go to `ctx.stream`
    /// — so an acquire costs two allocations and a hash lookup rather than a transfer.
    /// **The split happens only on a miss**, which is why `quadric_data` borrows its span instead of pinning it: a batch
    /// re-acquired every frame touches none of its bytes.
    /// The id is usable at once and the batch is `pending` until `collect_settled` sees its buffers land.
    /// Ray tracing need not be supported: nothing here builds an acceleration structure.
    [[nodiscard]] quadric_set_id acquire(quadric_data const& set);

    /// Marks every batch whose buffers have finished streaming `complete`; returns how many.
    /// A transfer that settled without delivering leaves its batch `failed` rather than pending.
    isize collect_settled();

    /// `id`'s procedural BLAS, recorded onto `cmd` the first time a caller asks and held from then on.
    /// Same contract as `mesh_manager::acquire_blas`: `id` must be `complete`, and ray tracing supported on the context.
    [[nodiscard]] sg::blas_handle const& acquire_blas(sg::command_list& cmd, quadric_set_id id);

    /// Blocks until every queued transfer has landed, then finishes them all.
    /// For a caller with no frame loop to drain the queue — a test tracing what it just built, or a one-pass tool.
    void wait_for_settled();

    /// How many batches are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

private:
    explicit quadric_manager(sg::context& ctx) : _ctx(ctx) {}

    /// The transfers one queued batch is waiting on.
    /// The payload is NOT held here: `ctx.stream` took the pins, so dropping this entry cancels whatever has not been sent —
    /// which is what makes evicting a still-streaming batch free.
    struct pending_set
    {
        sg::stream_upload_handle primitives;
        sg::stream_upload_handle aabbs;
        isize bytes = 0;
    };

    /// Finishes `id` if its transfers have settled; returns whether it did.
    [[nodiscard]] bool _try_settle(quadric_set_id id, pending_set& p);

    sg::context& _ctx;
    cc::map<quadric_set_id, pending_set> _settling;
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

    /// What the elements hold, read at the acquire that uploaded them; empty for a format other than f32 lanes.
    /// An emissive instance whose emission reads this attribute is weighed by its mean, and is no light at all where it is dark throughout.
    cc::optional<value_summary> summary;
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

    /// Whether `texture` is the BC7 encoding that replaced what was uploaded.
    bool compressed = false;

    /// What the smallest level the acquire supplied holds, as the sampler reads it; empty for a format `summarize_texels` does not decode.
    /// Only a summary of the base level read whole is exact, since a smaller level's range need not bound the base's.
    /// `replace_with_compressed` makes it inexact, since it summarizes the pixels before their lossy encoding.
    /// An emissive instance whose emission samples this texture is weighed by its mean.
    cc::optional<value_summary> summary;
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

    /// Puts `compressed` behind `id` in place of what was uploaded, and charges the budget what it costs instead.
    /// `compressed` holds the same chain, encoded; a no-op for an id that has been evicted since.
    void replace_with_compressed(texture_id id, sg::texture_2d compressed);

private:
    explicit texture_manager(sg::context& ctx) : _ctx(ctx) {}

    sg::context& _ctx;

    /// The in-flight transfers per queued texture, one per supplied mip; dropping them cancels the upload.
    cc::map<texture_id, cc::vector<sg::stream_upload_handle>> _settling;
};
