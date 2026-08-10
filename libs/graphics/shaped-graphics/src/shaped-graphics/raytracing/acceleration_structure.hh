#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh> // tlas_view (tlas::as_view)

#include <atomic>
#include <memory>

/// Ray-tracing acceleration structures: the opaque, driver-built spatial indices the GPU traverses to find ray/geometry hits.
/// A `blas` (bottom-level) indexes one mesh's triangles or procedural primitives.
/// A `tlas` (top-level) indexes a set of instances, each placing a `blas` into the world with a transform.
/// Built through `cmd.raytracing.build_blas(...)` / `build_tlas(...)` (command_list/raytracing.hh).
/// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

namespace sg
{
/// Trade-offs baked into a structure at build time, which cannot change afterward.
/// Bit flags — combine with `|`, test with `has_flag`.
/// `fast_trace` and `fast_build` must not both be set.
/// Migrates to `cc::flags`, same status as buffer_usage.
enum class accel_build_flags : u32
{
    none = 0,
    fast_trace = 1u << 0,       ///< optimize traversal speed (the default): DX12/Vk PREFER_FAST_TRACE
    fast_build = 1u << 1,       ///< optimize build speed:                   DX12/Vk PREFER_FAST_BUILD
    allow_update = 1u << 2,     ///< permit a later refit:                   DX12/Vk ALLOW_UPDATE
    allow_compaction = 1u << 3, ///< BLAS only — copy to a smaller buffer later: DX12/Vk ALLOW_COMPACTION
    minimize_memory = 1u << 4,  ///< TLAS only — smaller scratch/result:     DX12/Vk MINIMIZE_MEMORY
};

[[nodiscard]] constexpr accel_build_flags operator|(accel_build_flags a, accel_build_flags b)
{
    return accel_build_flags(u32(a) | u32(b));
}
[[nodiscard]] constexpr accel_build_flags operator&(accel_build_flags a, accel_build_flags b)
{
    return accel_build_flags(u32(a) & u32(b));
}

/// True if every bit in `flag` is set in `flags`.
[[nodiscard]] constexpr bool has_flag(accel_build_flags flags, accel_build_flags flag)
{
    return (u32(flags) & u32(flag)) == u32(flag);
}

/// One triangle geometry in a BLAS, indexed when an index buffer is given.
/// Every buffer referenced here must carry buffer_usage::accel_structure_build_input.
struct blas_triangles
{
    /// float3 positions, where `stride_in_bytes` allows interleaved vertex structs and `offset_in_bytes` selects a sub-range.
    /// On a non-indexed geometry `vertex_count` must be a multiple of 3; an indexed one constrains `index_count` instead.
    raw_buffer_handle vertices = nullptr;
    isize vertex_count = 0;
    isize vertex_stride_in_bytes = isize(sizeof(float) * 3);
    isize vertex_offset_in_bytes = 0;

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
};

/// One procedural geometry in a BLAS: a list of axis-aligned bounding boxes an intersection shader refines.
/// A BLAS is triangles *or* AABBs, never both, which is enforced by the build_blas overload chosen.
struct blas_aabbs
{
    /// Buffer of `D3D12_RAYTRACING_AABB`-shaped records — 6 floats, min.xyz then max.xyz.
    /// `stride_in_bytes` must be a multiple of 8, and the buffer must carry buffer_usage::accel_structure_build_input.
    raw_buffer_handle aabbs = nullptr;
    isize aabb_count = 0;
    isize aabb_stride_in_bytes = isize(sizeof(float) * 6);
    isize aabb_offset_in_bytes = 0;
    bool is_opaque = true;
};

/// Winding-based triangle cull selection for a TLAS instance.
/// `none` disables triangle culling, while `back` and `front` differ by winding — `front` flips it.
/// The final cull also depends on the ray flags at trace time.
enum class instance_cull_mode : u8
{
    back,  ///< cull back faces (default winding) — no instance flag
    front, ///< cull front faces — sets the front-counterclockwise flag to flip winding
    none,  ///< disable triangle culling — sets the cull-disable flag
};

/// One TLAS instance: places a built BLAS into the world.
/// Holding the blas_handle is the ownership edge, so the referenced BLAS outlives every TLAS that names it.
struct tlas_instance
{
    /// The BLAS this instance places.
    /// Must be non-null and fully built before build_tlas.
    blas_handle blas = nullptr;

    /// World transform, **row-major 3×4** affine: element (row r, col c) is `transform[r * 4 + c]`, and the translation is column 3.
    /// This is the DXR / Vulkan wire layout, and is deliberately row-major unlike typed-geometry's column-major `tg::mat`.
    /// So build this array explicitly rather than memcpy'ing a `tg::mat`.
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    /// Surfaced to the shader as InstanceID() / gl_InstanceCustomIndex.
    /// Must fit in 24 bits.
    u32 instance_id = 0;

    /// InstanceContributionToHitGroupIndex — the base hit-group index for this instance.
    /// Must fit in 24 bits.
    u32 hit_group_offset = 0;

    /// 8-bit visibility mask AND-ed against the ray's mask, where 0 means never hit.
    u8 mask = 0xFF;

    instance_cull_mode cull_mode = instance_cull_mode::back;

    /// Optional per-instance opaque override: unset uses each geometry's own flag.
    /// A value forces the whole instance opaque or non-opaque — DX12 FORCE_OPAQUE / FORCE_NON_OPAQUE.
    cc::optional<bool> opaque_override = {};
};

/// A bottom-level acceleration structure: an opaque, driver-built index over one mesh's triangles or procedural primitives.
/// A vocabulary type with no typed wrapper, held via blas_handle.
/// Abstract: a backend subclasses it and owns the native object, while the single accel_structure_storage buffer and the cheap stats live here.
/// Built through cmd.raytracing.build_blas, and the returned handle is persistent — valid across epochs.
class blas : public std::enable_shared_from_this<blas>
{
public:
    virtual ~blas();

    /// The single opaque accel_structure_storage buffer holding the built structure.
    [[nodiscard]] raw_buffer_handle storage() const { return _storage; }
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Scratch this structure needed at build time, and separately at update / refit time.
    /// Retained so a future refit can size scratch without re-querying, and zero until a backend fills it.
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }
    [[nodiscard]] isize update_scratch_size_in_bytes() const { return _update_scratch_size_in_bytes; }

    [[nodiscard]] accel_build_flags build_flags() const { return _build_flags; }
    [[nodiscard]] int geometry_count() const { return _geometry_count; }
    [[nodiscard]] bool allows_update() const { return has_flag(_build_flags, accel_build_flags::allow_update); }

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
    blas(raw_buffer_handle storage,
         isize size_in_bytes,
         isize build_scratch_size_in_bytes,
         isize update_scratch_size_in_bytes,
         accel_build_flags build_flags,
         int geometry_count);

    /// Backend hook run once from expire().
    /// The default expires the storage buffer, releasing its GPU memory; a backend may override to also drop native objects it holds.
    virtual void on_expired() const;

    raw_buffer_handle _storage;
    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    isize _update_scratch_size_in_bytes = 0;
    accel_build_flags _build_flags = accel_build_flags::none;
    int _geometry_count = 0;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired = {false};                // mutable: expire() is a const lifetime hook
};

/// A top-level acceleration structure: an opaque index over a set of instances, each placing a blas with a transform.
/// This is what a ray tracer traces against.
/// A vocabulary type held via tlas_handle, and abstract like blas, with the storage and stats here.
/// Built through cmd.raytracing.build_tlas, and the returned handle is persistent — valid across epochs.
/// A tlas keeps every referenced blas alive.
class tlas : public std::enable_shared_from_this<tlas>
{
public:
    virtual ~tlas();

    [[nodiscard]] raw_buffer_handle storage() const { return _storage; }
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }
    [[nodiscard]] isize update_scratch_size_in_bytes() const { return _update_scratch_size_in_bytes; }
    [[nodiscard]] accel_build_flags build_flags() const { return _build_flags; }
    [[nodiscard]] int instance_count() const { return _instance_count; }
    [[nodiscard]] bool allows_update() const { return has_flag(_build_flags, accel_build_flags::allow_update); }

    /// A shader-bindable view of this TLAS — HLSL `RaytracingAccelerationStructure`.
    /// Pass it into a binding_group like any other view; the view carries this tlas, which each backend binds its own way.
    /// A dispatch that binds it declares `accel_read` on the tlas storage.
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
    tlas(raw_buffer_handle storage,
         isize size_in_bytes,
         isize build_scratch_size_in_bytes,
         isize update_scratch_size_in_bytes,
         accel_build_flags build_flags,
         int instance_count,
         cc::vector<blas_handle> referenced_blases);

    virtual void on_expired() const;

    raw_buffer_handle _storage;
    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    isize _update_scratch_size_in_bytes = 0;
    accel_build_flags _build_flags = accel_build_flags::none;
    int _instance_count = 0;
    cc::vector<blas_handle> _referenced_blases; // the ownership edge: keeps every referenced BLAS alive
    mutable cc::vector<cc::unique_function<void()>> _finalizers;
    mutable std::atomic<bool> _expired = {false};
};
} // namespace sg
