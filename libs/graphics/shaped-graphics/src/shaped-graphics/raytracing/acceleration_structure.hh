#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/micromap.hh> // blas_triangles::micromap
#include <shaped-graphics/resource/views.hh>      // tlas_view (tlas::as_view)

#include <atomic>

/// Ray-tracing acceleration structures: the opaque, driver-built spatial indices the GPU traverses to find ray/geometry hits.
/// A `blas` (bottom-level) indexes one mesh's triangles or procedural primitives.
/// A `tlas` (top-level) indexes a set of instances, each placing a `blas` into the world with a transform.
/// Built through `cmd.raytracing.build_blas(...)` / `build_tlas(...)` (command_list/raytracing.hh).
/// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

/// One trade-off baked into a structure at build time, which cannot change afterward.
/// A set of them is an accel_build_flags — combine with `|`, test with `has`.
/// `fast_trace` and `fast_build` must not both be set.
enum class sg::accel_build_flag
{
    fast_trace,       ///< optimize traversal speed (the default): DX12/Vk PREFER_FAST_TRACE
    fast_build,       ///< optimize build speed:                   DX12/Vk PREFER_FAST_BUILD
    allow_update,     ///< permit a later refit:                   DX12/Vk ALLOW_UPDATE
    allow_compaction, ///< BLAS only — copy to a smaller buffer later: DX12/Vk ALLOW_COMPACTION
    software_trace, ///< dx12 and vulkan: also build the software walk's structure, from the description's `cpu_*` spans
    minimize_memory, ///< TLAS only — smaller scratch/result:     DX12/Vk MINIMIZE_MEMORY
};

CC_FLAG_ENUM_INDEXED(sg, accel_build_flag, u32);

namespace sg
{
/// A SET of accel_build_flag — what a build takes and a structure stores, never the bare enum.
/// The empty set asks for no trade-off at all, which the driver reads as its own defaults.
using accel_build_flags = cc::flags<accel_build_flag>;
} // namespace sg

/// Where a moving geometry's vertices stand at a later time: a buffer laid out as `blas_triangles::vertices`, with its stride and count.
/// It must carry buffer_usage::accel_structure_build_input.
struct sg::blas_pose
{
    raw_buffer_handle vertices = nullptr;
    isize offset_in_bytes = 0;
    /// What `vertices` holds, from byte 0, for `accel_build_flag::software_trace`; see blas_triangles::cpu_vertices.
    cc::span<byte const> cpu_vertices = {};
};

/// One triangle geometry in a BLAS, indexed when an index buffer is given.
/// Every buffer referenced here must carry buffer_usage::accel_structure_build_input.
struct sg::blas_triangles
{
    /// float3 positions, where `stride_in_bytes` allows interleaved vertex structs and `offset_in_bytes` selects a sub-range.
    /// On a non-indexed geometry `vertex_count` must be a multiple of 3; an indexed one constrains `index_count` instead.
    raw_buffer_handle vertices = nullptr;
    isize vertex_count = 0;
    isize vertex_stride_in_bytes = isize(sizeof(float) * 3);
    isize vertex_offset_in_bytes = 0;

    /// The geometry's poses after `vertices`, evenly spaced with the last at time 1, the topology staying the same.
    /// A ray at time `s` meets each vertex at `v0 + (v1 - v0) * s`, on its own line (SGL's EVAL-132).
    /// Empty, the geometry stands still; otherwise it requires `sg::feature::ray_tracing_motion_blur`.
    /// At most one pose today, which is what VK_NV_ray_tracing_motion_blur takes; Metal's motion takes more.
    cc::vector<blas_pose> later_poses = {};

    /// Optional index buffer; null means non-indexed.
    /// When set, `index_count` must be a multiple of 3, and `index_type` selects the element width.
    raw_buffer_handle indices = nullptr;
    isize index_count = 0;
    isize index_offset_in_bytes = 0;
    index_format index_type = index_format::uint32;

    /// Optional per-geometry transform: a buffer holding one 3×4 **row-major** float matrix, 48 bytes, at `transform_offset_in_bytes`.
    /// A buffer reference, since DXR reads it by GPU address, unlike the inline per-instance transform.
    /// Null means identity.
    raw_buffer_handle transform = nullptr;
    isize transform_offset_in_bytes = 0;

    /// Geometry is opaque, so no any-hit runs: DX12 GEOMETRY_FLAG_OPAQUE / Vk GEOMETRY_OPAQUE.
    /// Defaulted on as the common fast case; clear it for alpha-tested geometry.
    bool is_opaque = true;

    /// What `vertices`, `indices` and `transform` hold, from byte 0 of each, for `accel_build_flag::software_trace`.
    /// A build on dx12 and vulkan reads these within the call, on the CPU, and only them: sg reads no GPU buffer back.
    /// The offsets above apply to them as to the buffers.
    cc::span<byte const> cpu_vertices = {};
    cc::span<byte const> cpu_indices = {};
    cc::span<byte const> cpu_transform = {};

    /// Optional opacity micromap, which must be null where the context lacks `sg::feature::opacity_micromap`.
    /// `micromap_indices` then holds one index per triangle: a map of `micromap`, or a `micromap_special_index`.
    /// An unsigned `micromap_index_type` reads its bits as signed, so `0xFFFFFFFF` is `fully_transparent`.
    micromap_handle micromap = nullptr;
    raw_buffer_handle micromap_indices = nullptr;
    isize micromap_indices_offset_in_bytes = 0;
    index_format micromap_index_type = index_format::uint32;

    /// How many triangles name a map of each level and format.
    /// Empty means `micromap`'s own usage, which is right where every map is named by exactly one triangle.
    cc::vector<micromap_usage> micromap_usage_counts = {};
};

/// One procedural geometry in a BLAS: a list of axis-aligned bounding boxes an intersection shader refines.
/// A BLAS is triangles *or* AABBs, never both, which is enforced by the build_blas overload chosen.
struct sg::blas_aabbs
{
    /// Buffer of `D3D12_RAYTRACING_AABB`-shaped records — 6 floats, min.xyz then max.xyz.
    /// `stride_in_bytes` must be a multiple of 8, and the buffer must carry buffer_usage::accel_structure_build_input.
    raw_buffer_handle aabbs = nullptr;
    isize aabb_count = 0;
    isize aabb_stride_in_bytes = isize(sizeof(float) * 6);
    isize aabb_offset_in_bytes = 0;
    bool is_opaque = true;

    /// What `aabbs` holds, from byte 0, for `accel_build_flag::software_trace`; see blas_triangles::cpu_vertices.
    cc::span<byte const> cpu_aabbs = {};
};

/// Winding-based triangle cull selection for a TLAS instance.
/// `none` disables triangle culling, while `back` and `front` differ by winding — `front` flips it.
/// The final cull also depends on the ray flags at trace time.
enum class sg::instance_cull_mode : sg::u8
{
    back,  ///< cull back faces (default winding) — no instance flag
    front, ///< cull front faces — sets the front-counterclockwise flag to flip winding
    none,  ///< disable triangle culling — sets the cull-disable flag
};

/// What a BLAS was built from, which fixes the kind of hit group every record it reaches must be.
enum class sg::blas_geometry : sg::u8
{
    triangles, ///< built from blas_triangles; the records it reaches must carry no intersection shader
    aabbs,     ///< built from blas_aabbs; the records it reaches must carry one
};

/// One TLAS instance: places a built BLAS into the world.
/// Holding the blas_handle is the ownership edge, so the referenced BLAS outlives every TLAS that names it.
struct sg::tlas_instance
{
    /// The BLAS this instance places.
    /// Must be non-null and fully built before build_tlas.
    blas_handle blas = nullptr;

    /// World transform, **row-major 3×4** affine: element (row r, col c) is `transform[r * 4 + c]`, and the translation is column 3.
    /// This is the DXR / Vulkan wire layout, and is deliberately row-major unlike typed-geometry's column-major `tg::mat`.
    /// So build this array explicitly rather than memcpy'ing a `tg::mat`.
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    /// Where `transform` has moved to at the end of the frame's interval, in its layout: a ray at time `s` meets the
    /// instance under `transform + (transform_at_end - transform) * s`, element by element (SGL's EVAL-130).
    /// Unset, the instance stands still; set, it requires `sg::feature::ray_tracing_motion_blur`.
    cc::optional<cc::fixed_array<float, 12>> transform_at_end = {};

    /// Surfaced to the shader as InstanceID() / gl_InstanceCustomIndex.
    /// Must fit in 24 bits.
    u32 instance_id = 0;

    /// InstanceContributionToHitGroupIndex — the base hit-group index for this instance.
    /// Geometry g of the BLAS then reads record `hit_group_offset + g * multiplier + ray_type`, and a shader table's `offset_of(row)` is the value for a row.
    /// Must fit in 24 bits.
    u32 hit_group_offset = 0;

    /// 8-bit visibility mask AND-ed against the ray's mask, where 0 means never hit.
    u8 mask = 0xFF;

    instance_cull_mode cull_mode = instance_cull_mode::back;

    /// Optional per-instance opaque override: unset uses each geometry's own flag.
    /// A value forces the whole instance opaque or non-opaque — DX12 FORCE_OPAQUE / FORCE_NON_OPAQUE.
    cc::optional<bool> opaque_override = {};
};

namespace sg::impl
{
/// A structure's region of a context's software pool, where a build with `accel_build_flag::software_trace` wrote one.
struct software_region
{
    software_acceleration_pool* pool = nullptr; ///< null where the build wrote none
    u32 unit = 0;
    u32 units = 0;
    /// It holds or reaches something that moves, which only a trace of the software walk can see.
    bool is_moving = false;
    /// What a TLAS over it needs of a BLAS: its root box.
    float box_min[3] = {};
    float box_max[3] = {};
};

/// What the hit-record check of `context::hit_record_checks` needs of one tlas instance.
struct tlas_instance_record
{
    blas_handle blas;
    u32 hit_group_offset = 0;
    u8 mask = 0xFF;
};

/// Records what the build took, as the structure comes back from its backend and before anyone else holds it.
void set_build_record(blas const& blas, blas_geometry geometry, int hit_record_stride);

/// Records every instance of `tlas`, which only a build or a partitioned update under `context::hit_record_checks` does.
/// A later call replaces the snapshot, and one a check already holds stays as it was.
void set_instance_records(tlas const& tlas, cc::vector<tlas_instance_record> records);

/// Records the micromaps a BLAS was built from, which it then holds.
void set_micromaps(blas const& blas, cc::vector<micromap_handle> micromaps);

/// Records the cluster set a cluster BLAS was built from, which it then holds.
void set_clusters(blas const& blas, cluster_set_handle clusters);

/// Drops every instance record of `tlas`, so no check reads it; a snapshot a check already holds stays as it was.
void clear_instance_records(tlas const& tlas);

/// A cluster pool the recorded instances of a tlas reach, at the generation their BLASes were built from.
struct reached_pool
{
    cluster_set_handle pool;
    u64 generation = 0;
};

/// Whether a pool the recorded instances of `tlas` reach was rebuilt since, so a trace would meet clusters that are gone.
/// Always false where `set_instance_records` never ran.
[[nodiscard]] bool reaches_rebuilt_pool(tlas const& tlas);

/// What `set_instance_records` recorded last, which is null where it never ran.
[[nodiscard]] std::shared_ptr<cc::vector<tlas_instance_record> const> instance_records_of(tlas const& tlas);

/// Records the region a build wrote, which the structure then releases.
void set_software_region(blas const& blas, software_region region);
void set_software_region(tlas const& tlas, software_region region);
[[nodiscard]] software_region const& software_region_of(blas const& blas);
[[nodiscard]] software_region const& software_region_of(tlas const& tlas);
} // namespace sg::impl

/// A bottom-level acceleration structure: an opaque, driver-built index over one mesh's triangles or procedural primitives.
/// A vocabulary type with no typed wrapper, held via blas_handle.
/// Abstract: a backend subclasses it and owns the native structure, while the cheap stats live here.
/// What a built structure *is* differs per backend — a buffer on DXR, an object of its own on Metal — so the base holds no handle to it.
/// Built through cmd.raytracing.build_blas, and the returned handle is persistent — valid across epochs.
class sg::blas : public std::enable_shared_from_this<blas>
{
public:
    virtual ~blas();

    /// Bytes the built structure occupies.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Scratch this structure needed at build time, and separately at update / refit time.
    /// Retained so a future refit can size scratch without re-querying, and zero until a backend fills it.
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }
    [[nodiscard]] isize update_scratch_size_in_bytes() const { return _update_scratch_size_in_bytes; }

    [[nodiscard]] accel_build_flags build_flags() const { return _build_flags; }
    [[nodiscard]] int geometry_count() const { return _geometry_count; }
    [[nodiscard]] bool allows_update() const { return _build_flags.has(accel_build_flag::allow_update); }

    /// Whether the build took triangles or AABBs.
    [[nodiscard]] blas_geometry geometry() const { return _geometry; }

    /// Whether this is a cluster BLAS whose pool was rebuilt since, so the clusters it lists are gone.
    [[nodiscard]] bool is_stale() const;

    /// The shader-table records one geometry of this BLAS takes, as build_blas was given it.
    /// It is the ray count of the pipelines that trace it, and only metal bakes it into the structure.
    [[nodiscard]] int hit_record_stride() const { return _hit_record_stride; }

    /// Registers a callback to run once this structure is released and no longer in flight — see raw_buffer.
    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }

    /// Whether this structure's storage has been reclaimed.
    /// Once true, it never goes back to false.
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the structure now, releasing its GPU storage — deferred until it is no longer in flight, and idempotent.
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    blas(isize size_in_bytes,
         isize build_scratch_size_in_bytes,
         isize update_scratch_size_in_bytes,
         accel_build_flags build_flags,
         int geometry_count);

    /// Backend hook run once from expire(), which is where the native structure and whatever backs it are released.
    /// Empty here: what a built structure *is* differs per backend, so the base owns none of it.
    virtual void on_expired() const {}

    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    isize _update_scratch_size_in_bytes = 0;
    accel_build_flags _build_flags = {};
    int _geometry_count = 0;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired = {false};                // mutable: expire() is a const lifetime hook

private:
    friend void impl::set_build_record(blas const& blas, blas_geometry geometry, int hit_record_stride);
    friend void impl::set_micromaps(blas const& blas, cc::vector<micromap_handle> micromaps);
    friend void impl::set_clusters(blas const& blas, cluster_set_handle clusters);
    friend void impl::set_instance_records(tlas const& tlas, cc::vector<impl::tlas_instance_record> records);
    friend void impl::set_software_region(blas const& blas, impl::software_region region);
    friend impl::software_region const& impl::software_region_of(blas const& blas);

    impl::software_region _software;
    blas_geometry _geometry = blas_geometry::triangles;
    int _hit_record_stride = 1;
    cc::vector<micromap_handle> _micromaps; // the ownership edge: keeps every micromap a geometry named alive
    cluster_set_handle _clusters;           // the ownership edge of a cluster BLAS: keeps the clusters it lists alive
    u64 _cluster_generation = 0;            // the generation of `_clusters` the BLAS was built from
};

/// A top-level acceleration structure: an opaque index over a set of instances, each placing a blas with a transform.
/// This is what a ray tracer traces against.
/// A vocabulary type held via tlas_handle, and abstract like blas, with the stats here and the native structure in the subclass.
/// Built through cmd.raytracing.build_tlas, and the returned handle is persistent — valid across epochs.
/// A tlas keeps every referenced blas alive.
class sg::tlas : public std::enable_shared_from_this<tlas>
{
public:
    virtual ~tlas();

    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }
    [[nodiscard]] isize update_scratch_size_in_bytes() const { return _update_scratch_size_in_bytes; }
    [[nodiscard]] accel_build_flags build_flags() const { return _build_flags; }
    [[nodiscard]] int instance_count() const { return _instance_count; }
    [[nodiscard]] bool allows_update() const { return _build_flags.has(accel_build_flag::allow_update); }

    /// A shader-bindable view of this TLAS — HLSL `RaytracingAccelerationStructure`.
    /// Pass it into a binding_group like any other view; the view carries this tlas, which each backend binds its own way.
    /// A dispatch that binds it declares `accel_read` on the tlas.
    [[nodiscard]] tlas_view as_view() const { return tlas_view{.tlas = shared_from_this()}; }

    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_valid() const { return !is_expired(); }
    void expire() const
    {
        if (!_expired.exchange(true, std::memory_order_acq_rel))
            on_expired();
    }

protected:
    tlas(isize size_in_bytes,
         isize build_scratch_size_in_bytes,
         isize update_scratch_size_in_bytes,
         accel_build_flags build_flags,
         int instance_count,
         cc::vector<blas_handle> referenced_blases);

    virtual void on_expired() const {}

    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    isize _update_scratch_size_in_bytes = 0;
    accel_build_flags _build_flags = {};
    int _instance_count = 0;
    cc::vector<blas_handle> _referenced_blases; // the ownership edge: keeps every referenced BLAS alive
    mutable cc::vector<cc::unique_function<void()>> _finalizers;
    mutable std::atomic<bool> _expired = {false};

private:
    friend void impl::set_instance_records(tlas const& tlas, cc::vector<impl::tlas_instance_record> records);
    friend std::shared_ptr<cc::vector<impl::tlas_instance_record> const> impl::instance_records_of(tlas const& tlas);
    friend void impl::clear_instance_records(tlas const& tlas);
    friend bool impl::reaches_rebuilt_pool(tlas const& tlas);
    friend void impl::set_software_region(tlas const& tlas, impl::software_region region);
    friend impl::software_region const& impl::software_region_of(tlas const& tlas);

    impl::software_region _software;
    // mutable: a partitioned tlas replaces the snapshot at each update, through a const handle
    mutable cc::mutex<std::shared_ptr<cc::vector<impl::tlas_instance_record> const>> _instance_records;
    mutable cc::mutex<cc::vector<impl::reached_pool>> _reached_pools; // of the records, set and cleared with them
};
