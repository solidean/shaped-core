#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh> // cc::span
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/transfer/stream_handle.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <shaped-viewer/resources/impl/range_allocator.hh>
#include <shaped-viewer/resources/quadric_data.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/value_summary.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format / attribute_frequency, which a record carries
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/mat.hh>
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

/// Where one mesh's triangles sit in `mesh_manager`'s two central pools.
///
/// Its vertices are `[base_vertex, base_vertex + vertex_count)` of `vertex_pool`.
/// Its indices are `[first_index, first_index + 3 * triangle_count)` of `index_pool`, **relative to `base_vertex`**.
/// So an index is the vertex a per-vertex attribute is numbered by, and `base_vertex` is added only to reach a position.
/// That is also what an indexed draw's base vertex and a BLAS's vertex offset expect.
struct sv::geometry_range
{
    isize base_vertex = 0;
    isize vertex_count = 0;
    isize first_index = 0;
    isize triangle_count = 0;

    [[nodiscard]] isize index_count() const { return 3 * triangle_count; }
};

/// One uploaded mesh: where its geometry sits in the manager's central pools, and the BLAS built from it once something traces it.
///
/// **The BLAS is built on demand**, by the first trace that reaches the mesh through `mesh_manager::acquire_blas`.
/// A mesh only ever rasterized never gets one, which is what lets a raster layer draw on a device without ray tracing.
/// Once built it stays, so a trace then rebuilds only the (cheap) TLAS each frame.
///
/// **Every mesh is indexed on the GPU.**
/// A raw triangle list is uploaded with the indices 0, 1, 2, ..., so its triangle `t` still has the corners `3t`, `3t + 1` and `3t + 2`, which a per-vertex attribute is numbered by.
struct sv::mesh_record
{
    /// How much of this mesh has reached the GPU: `complete` once its geometry is uploaded, whether or not a BLAS was built.
    /// While `pending` there is nothing to draw, so a placeholder box stands in for it — see `mesh_manager::placeholder_geometry`.
    residency state = residency::pending;

    geometry_range geometry;

    /// Null until a trace asks `mesh_manager::acquire_blas` for it.
    sg::blas_handle blas;

    /// The object-space surface area, summed over the triangles on the acquire that uploaded them.
    /// What an emissive instance's selection weight starts from, so it is known before any CDF is built.
    f32 surface_area = 0;

    /// The area CDF next-event estimation picks one of the mesh's triangles by, null until a trace asks `mesh_manager::acquire_emitter_cdf` for it.
    /// A mesh no emissive instance places never gets one.
    /// `triangle_count + emitter_cdf_block_count(triangle_count) + 1` floats, laid out as shaders/emitter_cdf.sgl says.
    sg::buffer<f32> emitter_cdf;

    /// One placement's world area, as `mesh_manager::acquire_world_area` sums it on the GPU.
    struct world_area
    {
        /// The linear map it is the area under, which is the whole key: a translation moves no area.
        tg::mat3f linear;
        /// The GPU's sums until they land, then nothing.
        sg::data_future<f32> pending;
        cc::optional<f32> area;
    };

    /// The placements asked for most recently, the most recently asked last and at most `mesh_manager::max_world_areas` of them.
    cc::vector<world_area> world_areas;

    /// The object-space box, when the payload declared one — the summary half of the record.
    ///
    /// It outlives the payload, so a pending mesh is drawn as the shared placeholder box scaled onto
    /// this, and a mesh whose bounds nobody stated is skipped instead, since guessing an extent would place it wrong.
    cc::optional<tg::aabb3f> bounds;
};

/// Hands out `mesh_id`s and owns the geometry + BLAS behind each, with LRU budgeting (see resource_budget).
/// This is where the one-time BLAS build lives (a BLAS build is setup, not a per-frame pass, so it stays
/// here rather than in a routine), and it runs only for a mesh something traces.
///
/// **Every mesh's geometry lives in two central pools**, one vertex buffer and one index buffer, each mesh a range of both.
/// A quadric batch's records and boxes are blocks of the vertex pool too, `allocate_block`, so one bound buffer reaches
/// every kind of geometry, which is what the bound raster mode has instead of bindless.
/// That is what lets a draw be indexed without a buffer bound per mesh, and a BLAS be built from the very bytes a draw reads.
/// A mesh streams into staging of its own and is copied into its ranges once it landed, so no stream ever targets a pool.
/// A pool that runs out is replaced by one twice its size, its contents copied over on the GPU.
/// An evicted mesh's ranges are reused only once the frames that may still read them have retired.
class sv::mesh_manager : public impl::lru_pool<mesh_id, mesh_record>
{
public:
    /// A range of the vertex pool holding something other than a mesh's positions, in its 12-byte elements.
    struct pool_block
    {
        /// Where the block starts, a multiple of the alignment it was asked for.
        isize offset = 0;
        isize count = 0;

        /// The range taken from the allocator, which the alignment may have widened; what `retire_block` frees.
        isize taken_offset = 0;
        isize taken_count = 0;
    };

    /// A manager that records every acquire into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static mesh_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The mesh_id for `mesh.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// On a miss the mesh's ranges are allocated and the geometry is handed to `ctx.stream`, which carries it on the copy
    /// queue at its own pace — so an acquire costs an allocation and a hash lookup rather than a transfer.
    /// The id is usable at once and the mesh is `pending` until `collect_settled` sees its geometry land; until then it
    /// is drawn as the placeholder box.
    /// Ray tracing need not be supported: nothing here builds an acceleration structure.
    ///
    /// Non-indexed: `positions` is a triangle list, 3 vertices per triangle, count a multiple of 3; it is uploaded with the indices 0, 1, 2, ... .
    [[nodiscard]] mesh_id acquire(triangle_data const& mesh);
    /// Indexed: `indices` names triangles into `positions`, count a multiple of 3; nothing is de-indexed.
    [[nodiscard]] mesh_id acquire(indexed_triangle_data const& mesh);

    /// The unit cube every pending mesh is traced as, built on first use; ray tracing must be supported on the context.
    ///
    /// ONE BLAS for every placeholder in a scene: it spans `[0,1]^3`, and the extent a particular mesh should occupy
    /// is folded into its TLAS transform instead — so a hundred meshes still arriving cost one acceleration structure
    /// and a hundred 3x4 matrices.
    [[nodiscard]] sg::blas_handle const& placeholder_blas();

    /// Where the cube sits in the pools, which an instance record must name in place of a pending mesh's range.
    ///
    /// A hit reads positions back out of the instance to recompute the geometric normal, and a raster draw pulls its
    /// corners from it, so naming the real mesh's range while drawing the cube would read triangles it never had.
    /// Uploaded directly rather than streamed, so a list touching it never waits on a transfer.
    [[nodiscard]] geometry_range const& placeholder_geometry();

    /// The central vertex pool: every mesh's positions, `float3` each, at its `geometry_range::base_vertex`.
    ///
    /// The buffer is replaced when the pool grows, so read it per frame rather than keeping it.
    [[nodiscard]] sg::buffer<tg::pos3f> const& vertex_pool();

    /// The central index pool: every mesh's indices, `u32` each and relative to its base vertex, at its `geometry_range::first_index`.
    /// Replaced on growth like `vertex_pool`.
    [[nodiscard]] sg::buffer<u32> const& index_pool();

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
    /// Where the context's acceleration storage is full for now, it is `placeholder_blas()`, and a later call tries again.
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

    /// The most placements of one mesh `acquire_world_area` keeps, the one asked for least recently dropped first.
    /// Generous, since one lamp mesh is often placed many times over, and a placement dropped before its sum lands is summed again.
    static constexpr isize max_world_areas = 64;

    /// `id`'s exact world area under `linear`, or none until the GPU's sum has landed.
    ///
    /// The first ask for a placement records the sum onto `cmd` and reads it back; a later one returns it once it landed.
    /// Only a placement that is not a similarity needs it, since `impl::world_area_of` is exact for one.
    /// `id` must be `complete`, for the reason `acquire_blas` gives, and none ever lands once the kernels failed to build.
    [[nodiscard]] cc::optional<f32> acquire_world_area(sg::command_list& cmd, mesh_id id, tg::mat3f const& linear);

    /// Blocks until every queued transfer has landed, then finishes them all.
    ///
    /// For a caller with no frame loop to drain the queue — a test tracing what it just built, or a one-pass tool.
    /// The transfers are promoted first, so nothing depends on this call's own ordering.
    void wait_for_settled();

    /// How many meshes are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

    /// A block of `count` vertex-pool elements, 12 bytes each, starting at a multiple of `alignment` elements.
    /// The pool grows to fit it, as it does for a mesh.
    [[nodiscard]] pool_block allocate_block(isize count, isize alignment = 1);

    /// Frees `block` once the frames that may still read it have retired.
    void retire_block(pool_block const& block);

    /// How many vertices and indices the pools hold room for; they grow, and never shrink.
    [[nodiscard]] isize vertex_pool_capacity() const { return _vertex_ranges.capacity(); }
    [[nodiscard]] isize index_pool_capacity() const { return _index_ranges.capacity(); }

private:
    explicit mesh_manager(sg::context& ctx) : _ctx(ctx) {}

    /// One queued mesh: its payload streaming into staging buffers of its own, which `collect_settled` copies into its ranges once landed.
    ///
    /// Staged rather than streamed into the pools, because a list touching a buffer a stream still fills waits for the whole transfer, and every frame touches the pools.
    /// Dropping this entry cancels whatever has not been sent.
    struct pending_mesh
    {
        sg::stream_upload_handle vertices;
        sg::stream_upload_handle indices;
        sg::buffer<tg::pos3f> staged_vertices;
        sg::buffer<u32> staged_indices;
        isize bytes = 0;
    };

    /// An evicted mesh's ranges, waiting for the last frame that may read them to retire.
    struct retiring_ranges
    {
        geometry_range geometry;
        sg::epoch evicted_in = sg::epoch(0);
    };

    /// A retired block, waiting as an evicted mesh's ranges wait.
    struct retiring_block
    {
        pool_block block;
        sg::epoch evicted_in = sg::epoch(0);
    };

    void on_remove(mesh_id id, mesh_record& record) override;

    /// Creates both pools on first use, with the placeholder cube at their start.
    void _ensure_pools();

    /// Ranges for `vertex_count` vertices and `triangle_count` triangles, growing a pool that has no room.
    [[nodiscard]] geometry_range _allocate(isize vertex_count, isize triangle_count);

    /// Replaces a pool by one holding at least `min_capacity` elements, carrying its contents over.
    void _grow_vertex_pool(isize min_capacity);
    void _grow_index_pool(isize min_capacity);

    /// Allocates, streams and records one mesh; both acquires end here.
    [[nodiscard]] mesh_id _insert(cc::hash128 hash,
                                  cc::pinned_data<tg::pos3f const> positions,
                                  cc::pinned_data<u32 const> indices,
                                  f32 surface_area,
                                  cc::optional<tg::aabb3f> bounds);

    /// Frees every retiring range whose readers have all retired.
    void _reclaim();

    /// Finishes `id` if its transfers have settled, recording its copy into the pools onto `copies`, opened on first need; returns whether it did.
    [[nodiscard]] bool _try_settle(mesh_id id, pending_mesh& p, std::unique_ptr<sg::command_list>& copies);

    /// Whether the kernels of shaders/emitter_cdf.sgl are built, starting them on first use; polled, never waited on.
    [[nodiscard]] bool _emitter_cdf_kernels_ready();

    sg::context& _ctx;
    sg::async_compute_pipeline _cdf_blocks;
    sg::async_compute_pipeline _cdf_total;
    sg::async_compute_pipeline _world_blocks;
    sg::async_compute_pipeline _world_total;
    bool _cdf_failed = false;
    sg::blas_handle _placeholder_blas;
    bool _told_blas_pool_full = false;
    geometry_range _placeholder;
    sg::buffer<tg::pos3f> _vertex_pool;
    sg::buffer<u32> _index_pool;
    impl::range_allocator _vertex_ranges;
    impl::range_allocator _index_ranges;
    cc::vector<retiring_ranges> _retiring;
    cc::vector<retiring_block> _retiring_blocks;
    cc::map<mesh_id, pending_mesh> _settling;
};

/// One uploaded quadric batch: its two blocks of the mesh manager's vertex pool, and the procedural BLAS built over them
/// once something traces it.
///
/// Two blocks rather than one, because the hardware and the shader want different things.
/// `aabbs` is the acceleration structure's build input, and what a raster path draws each primitive as, read by the vertex stage.
/// `primitives` is what an intersection reads by the primitive index.
/// An instance record names the pool for both, `base_vertex` the records' block and `first_index` the boxes'.
///
/// The BLAS is built once, by the first trace that reaches the batch, exactly as a mesh's is: from then on only the (cheap) TLAS is
/// rebuilt per frame.
struct sv::quadric_set_record
{
    /// `complete` once both blocks hold their bytes, whether or not a BLAS was built.
    /// While `pending` there is nothing to trace, so the batch is drawn as the shared placeholder box scaled onto `bounds`.
    residency state = residency::pending;

    /// The intersection's own data, `quadric_gpu` records of 8 pool elements each, read by the primitive index.
    mesh_manager::pool_block primitives;

    /// The BLAS build input, one box of 2 pool elements per primitive, at an even element so each box is 8-byte aligned
    /// as D3D12 requires; its layout is `D3D12_RAYTRACING_AABB`, which `tg::aabb3f` already is.
    mesh_manager::pool_block aabbs;

    isize primitive_count = 0;

    /// Null until a trace asks `quadric_manager::acquire_blas` for it.
    sg::blas_handle blas;

    /// The extent in the batch's own frame, when the payload declared one — the summary half of the record.
    /// It outlives the payload, so a pending batch is drawn as the placeholder box scaled onto this, and one
    /// whose bounds nobody stated is skipped instead, since guessing an extent would place it wrong.
    cc::optional<tg::aabb3f> bounds;
};

/// Hands out `quadric_set_id`s and owns the two pool blocks + procedural BLAS behind each, with LRU budgeting.
///
/// The blocks are the mesh manager's, which every call that allocates, copies or frees one is handed: the two managers
/// sit side by side in `gpu_resource_manager`, which moves, so neither keeps a pointer to the other.
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
    /// On a miss the two blocks are allocated in `meshes`' vertex pool, the authored primitives are split into staging for
    /// them, and both payloads go to `ctx.stream` — so an acquire costs two allocations and a hash lookup rather than a transfer.
    /// **The split happens only on a miss**, which is why `quadric_data` borrows its span instead of pinning it: a batch
    /// re-acquired every frame touches none of its bytes.
    /// The id is usable at once and the batch is `pending` until `collect_settled` sees its buffers land.
    /// Ray tracing need not be supported: nothing here builds an acceleration structure.
    [[nodiscard]] quadric_set_id acquire(quadric_data const& set, mesh_manager& meshes);

    /// Marks every batch whose payload has finished streaming `complete`, its copy into `meshes`' pool submitted; returns how many.
    /// A transfer that settled without delivering leaves its batch `failed` rather than pending.
    /// Also hands the blocks of batches evicted since to `meshes` to retire.
    isize collect_settled(mesh_manager& meshes);

    /// `id`'s procedural BLAS, recorded onto `cmd` the first time a caller asks and held from then on.
    /// Same contract as `mesh_manager::acquire_blas`: `id` must be `complete`, and ray tracing supported on the context.
    [[nodiscard]] sg::blas_handle const& acquire_blas(sg::command_list& cmd, quadric_set_id id, mesh_manager& meshes);

    /// Blocks until every queued transfer has landed, then finishes them all.
    /// For a caller with no frame loop to drain the queue — a test tracing what it just built, or a one-pass tool.
    void wait_for_settled(mesh_manager& meshes);

    /// How many batches are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

private:
    explicit quadric_manager(sg::context& ctx) : _ctx(ctx) {}

    /// The transfers one queued batch is waiting on, into staging of its own that is copied into its blocks once landed.
    /// The payload is NOT held here: `ctx.stream` took the pins, so dropping this entry cancels whatever has not been sent —
    /// which is what makes evicting a still-streaming batch free.
    struct pending_set
    {
        sg::stream_upload_handle primitives;
        sg::stream_upload_handle aabbs;
        sg::buffer<tg::pos3f> staged_primitives;
        sg::buffer<tg::pos3f> staged_aabbs;
        isize bytes = 0;
    };

    void on_remove(quadric_set_id id, quadric_set_record& record) override;

    /// Finishes `id` if its transfers have settled, recording its copy into `meshes`' pool onto `copies`, opened on first need.
    [[nodiscard]] bool _try_settle(quadric_set_id id,
                                   pending_set& p,
                                   mesh_manager& meshes,
                                   std::unique_ptr<sg::command_list>& copies);

    sg::context& _ctx;
    cc::map<quadric_set_id, pending_set> _settling;

    /// The blocks of batches evicted since `collect_settled` last ran, which it hands to the mesh manager to retire.
    cc::vector<mesh_manager::pool_block> _released;
};

/// One uploaded mesh attribute: where its bytes sit in the manager's attribute pool, plus what a shader has to know to read them.
///
/// Elements are tightly packed from `offset`, so the descriptor a parameter block carries is this epoch's bindless index of
/// the pool, `offset`, and `format.size_bytes()` as the stride.
struct sv::attribute_record
{
    /// `pending` until the bytes are in the pool; only then does the record have a range, and may a descriptor name it.
    residency state = residency::pending;

    /// The elements' byte offset in `attribute_manager::pool()`, -1 while pending; fixed for the record's life once set.
    isize offset = -1;

    /// How many bytes the elements take, and so the range they hold in the pool.
    isize size_bytes = 0;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);
    attribute_frequency frequency = attribute_frequency::per_vertex;
    isize element_count = 0;

    /// What the elements hold, read at the acquire that uploaded them; empty for a format other than f32 lanes.
    /// An emissive instance whose emission reads this attribute is weighed by its mean.
    /// It is no light at all only where the summary is exact and dark throughout; past the elements a summary reads whole, it stays a light.
    cc::optional<value_summary> summary;
};

/// Hands out `attribute_id`s for arbitrary mesh attributes, with LRU budgeting (see resource_budget).
///
/// This is what makes the `mesh_attribute` rank of the material chain reach the GPU at all: any attribute a material resolves to,
/// at any format and frequency, rather than the four PBR names the old repack knew.
/// Keyed on the attribute's own `hash`, so a mesh re-acquired every frame costs one lookup.
///
/// **Every attribute lives in one pool**, each a range of it, read as `bytes` through one bindless slot or one bound buffer.
/// An attribute streams into staging of its own and is copied into its range once it lands, so no stream ever targets the pool.
/// A range is allocated only where a list then copies into it: when a stream settles, or when inline bytes are written.
/// So the pool grows only there, its old contents copied over ahead of every write into the new one.
/// An evicted range is reused only once the frames that may still read it have retired.
class sv::attribute_manager : public impl::lru_pool<attribute_id, attribute_record>
{
public:
    /// A manager that uploads into `ctx` (which must outlive it), budgeted by `cfg`.
    [[nodiscard]] static attribute_manager create(sg::context& ctx, manager_config const& cfg = {});

    /// The attribute_id for `attribute.hash`, resident from a prior acquire (O(1)), or a freshly queued one.
    ///
    /// A queued attribute is `pending` and has no range until `collect_settled` copies it in, so nothing may name it before.
    /// That is why the manager's queue is drained ahead of the mesh one: an attribute is up before the geometry it
    /// belongs to is drawn with anything but a placeholder.
    [[nodiscard]] attribute_id acquire(mesh_attribute const& attribute);

    /// The same for a frame mesh's attribute, whose content is new more often than not: nothing streams.
    /// Its bytes wait for `record_inline_uploads`, which writes them into the command list that draws it,
    /// so the attribute is `complete` in the frame it is placed rather than a frame or two after.
    /// A resident attribute of the same content is found as `acquire` finds it.
    [[nodiscard]] attribute_id acquire_inline(mesh_attribute const& attribute);

    /// Writes the bytes of every attribute acquired inline since the last call into `cmd`, and marks them `complete`.
    /// A read recorded later in `cmd` sees them; a renderer calls this before it records anything that reads an attribute.
    void record_inline_uploads(sg::command_list& cmd);

    /// How many attributes acquired inline wait for `record_inline_uploads`.
    [[nodiscard]] isize inline_count() const { return _inline_bytes.size(); }

    /// Copies every attribute whose bytes have finished streaming into its range and marks it `complete`; returns how many.
    /// The copies go on `cmd`, in order with what it already wrote into the pool, or on a list of their own submitted here.
    isize collect_settled(sg::command_list* cmd = nullptr);

    /// Blocks until every queued transfer has landed, then collects them.
    void wait_for_settled();

    /// How many attributes are still streaming.
    [[nodiscard]] isize settling_count() const { return _settling.size(); }

    /// The pool every resident attribute lives in, at its record's `offset`; empty until the first one landed.
    /// Replaced by a larger one when it fills, so read it per frame rather than keeping it.
    [[nodiscard]] sg::buffer<byte> const& pool() const { return _pool; }

private:
    explicit attribute_manager(sg::context& ctx) : _ctx(ctx) {}

    /// One queued attribute: the staging it streams into, and the transfer filling it; dropping one cancels it.
    struct pending_attribute
    {
        sg::buffer<byte> staging;
        sg::stream_upload_handle transfer;
    };

    /// An evicted attribute's range, waiting for the last frame that may read it to retire.
    struct retiring_range
    {
        isize offset = 0;
        isize size_bytes = 0;
        sg::epoch evicted_in = sg::epoch(0);
    };

    void on_remove(attribute_id id, attribute_record& record) override;

    /// Frees every retiring range whose epoch has completed.
    void _reclaim();

    /// A range of `bytes` in the pool, recording on `cmd` the copy of the old pool into a grown one when none fits.
    /// Everything this manager wrote into the pool before is recorded or submitted ahead of `cmd`'s next command.
    [[nodiscard]] isize _allocate(sg::command_list& cmd, isize bytes);

    sg::context& _ctx;

    sg::buffer<byte> _pool;
    impl::range_allocator _ranges;
    cc::vector<retiring_range> _retiring;

    cc::map<attribute_id, pending_attribute> _settling;

    /// Per attribute acquired inline, its bytes until a command list writes them; pinned, so holding them copies nothing.
    cc::map<attribute_id, cc::pinned_data<byte const>> _inline_bytes;
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
