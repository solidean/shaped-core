#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <memory>

/// Ray-tracing acceleration structures: the opaque, driver-built spatial indices the GPU traverses to find
/// ray/geometry hits. A `blas` (bottom-level) indexes one mesh's triangles or procedural primitives; a
/// `tlas` (top-level) indexes a set of instances, each placing a `blas` into the world with a transform.
/// Built through `cmd.raytracing.build_blas(...)` / `build_tlas(...)` (see command_list.raytracing.hh); see
/// libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

namespace sg
{
/// Index element width for an indexed-triangle geometry. Only meaningful when an index buffer is present.
enum class accel_index_format : u8
{
    uint16, ///< DXGI_FORMAT_R16_UINT / VK_INDEX_TYPE_UINT16
    uint32, ///< DXGI_FORMAT_R32_UINT / VK_INDEX_TYPE_UINT32
};

/// Trade-offs baked into a structure at build time; they cannot change afterward. Bit flags — combine with
/// `|`, test with `has_flag`. `fast_trace` and `fast_build` are mutually exclusive (setting both asserts).
/// Migrates to `cc::flags` once that lands (same status as buffer_usage).
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

/// One triangle geometry in a BLAS. Vertices are `float3` (the DXR∩Vulkan common denominator); an optional
/// index buffer makes it an indexed list. All referenced buffers must carry
/// buffer_usage::accel_structure_build_input.
struct blas_triangles
{
    /// float3 positions. `stride_in_bytes` allows interleaved vertex structs; `offset_in_bytes` selects a
    /// sub-range. Non-indexed => `vertex_count` must be a multiple of 3.
    raw_buffer_handle vertices = nullptr;
    isize vertex_count = 0;
    isize vertex_stride_in_bytes = isize(sizeof(float) * 3);
    isize vertex_offset_in_bytes = 0;

    /// Optional index buffer; null => non-indexed. When set, `index_count` must be a multiple of 3 and
    /// `index_format` selects the element width.
    raw_buffer_handle indices = nullptr;
    isize index_count = 0;
    isize index_offset_in_bytes = 0;
    accel_index_format index_format = accel_index_format::uint32;

    /// Optional per-geometry transform: a buffer holding one 3×4 **row-major** float matrix (48 bytes) at
    /// `transform_offset_in_bytes`. It is a buffer reference (DXR reads it by GPU address), unlike the inline
    /// per-instance transform. Null => identity. Must carry buffer_usage::accel_structure_build_input.
    raw_buffer_handle transform = nullptr;
    isize transform_offset_in_bytes = 0;

    /// Geometry is opaque (no any-hit invocation): DX12 GEOMETRY_FLAG_OPAQUE / Vk GEOMETRY_OPAQUE. Defaulted
    /// on — the common fast case; clear it for alpha-tested geometry.
    bool is_opaque = true;
};

/// One procedural geometry in a BLAS: a list of axis-aligned bounding boxes an intersection shader refines.
/// A BLAS is triangles *or* AABBs, never both (enforced by which build_blas overload is chosen).
struct blas_aabbs
{
    /// Buffer of `D3D12_RAYTRACING_AABB`-shaped records (6 floats: min.xyz, max.xyz). `stride_in_bytes` must
    /// be a multiple of 8. Must carry buffer_usage::accel_structure_build_input.
    raw_buffer_handle aabbs = nullptr;
    isize aabb_count = 0;
    isize aabb_stride_in_bytes = isize(sizeof(float) * 6);
    isize aabb_offset_in_bytes = 0;
    bool is_opaque = true;
};

/// Winding-based triangle cull selection for a TLAS instance. `none` disables triangle culling;
/// `back`/`front` differ by winding (`front` flips it). The final cull also depends on the ray flags at
/// trace time. Maps onto the common instance-flag denominator (DX12/Vk).
enum class instance_cull_mode : u8
{
    back,  ///< cull back faces (default winding) — no instance flag
    front, ///< cull front faces — sets the front-counterclockwise flag to flip winding
    none,  ///< disable triangle culling — sets the cull-disable flag
};

/// One TLAS instance: places a built BLAS into the world. Holding the blas_handle is the ownership edge —
/// the referenced BLAS outlives every TLAS that names it, and must be fully built before the TLAS build.
struct tlas_instance
{
    /// The BLAS this instance places. Must be non-null and fully built before build_tlas.
    blas_handle blas = nullptr;

    /// World transform, **row-major 3×4** affine: element (row r, col c) is `transform[r * 4 + c]`, and the
    /// translation is column 3 (`transform[3]`, `transform[7]`, `transform[11]`). NOTE: this is the DXR/Vulkan
    /// wire layout and is deliberately **row-major**, unlike typed-geometry's column-major `tg::mat` — build
    /// this array explicitly rather than memcpy'ing a `tg::mat`. Default: identity.
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    /// Surfaced to the shader as InstanceID() / gl_InstanceCustomIndex. 24-bit — asserts on overflow.
    u32 instance_id = 0;

    /// InstanceContributionToHitGroupIndex — base hit-group index for this instance. 24-bit — asserts on overflow.
    u32 hit_group_offset = 0;

    /// 8-bit visibility mask AND-ed against the ray's mask; 0 => never hit. Default: visible to all.
    u8 mask = 0xFF;

    instance_cull_mode cull_mode = instance_cull_mode::back;

    /// Optional per-instance opaque override: unset => use each geometry's flag; a value => force opaque
    /// (true) / force non-opaque (false) for the whole instance (DX12 FORCE_OPAQUE / FORCE_NON_OPAQUE).
    cc::optional<bool> opaque_override = {};
};

/// A bottom-level acceleration structure: an opaque, driver-built index over one mesh's triangles or
/// procedural primitives. Vocabulary type, held via blas_handle (shared-immutable). Abstract — a backend
/// subclasses it and owns the native object; the single accel_structure_storage buffer and cheap stats live
/// here. Built through cmd.raytracing.build_blas; the returned handle is persistent (valid across epochs).
class blas : public std::enable_shared_from_this<blas>
{
public:
    virtual ~blas();

    /// The single opaque accel_structure_storage buffer holding the built structure.
    [[nodiscard]] raw_buffer_handle storage() const { return _storage; }
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// Scratch this structure needed at build (and, separately, at update/refit) time — retained so a future
    /// refit can size scratch without re-querying. Zero until a backend fills it.
    [[nodiscard]] isize build_scratch_size_in_bytes() const { return _build_scratch_size_in_bytes; }
    [[nodiscard]] isize update_scratch_size_in_bytes() const { return _update_scratch_size_in_bytes; }

    [[nodiscard]] accel_build_flags build_flags() const { return _build_flags; }
    [[nodiscard]] int geometry_count() const { return _geometry_count; }
    [[nodiscard]] bool allows_update() const { return has_flag(_build_flags, accel_build_flags::allow_update); }

    /// Registers a callback to run once this structure is released and no longer in flight (see raw_buffer).
    void add_finalizer(cc::unique_function<void()> finalizer) const { _finalizers.push_back(cc::move(finalizer)); }

    /// Whether this structure's storage has been reclaimed. Once true, never goes back to false.
    [[nodiscard]] bool is_expired() const { return _expired.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_valid() const { return !is_expired(); }

    /// Expire the structure now, releasing its GPU storage (deferred until no longer in flight). Idempotent.
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

    /// Backend hook run once from expire(). Default expires the storage buffer (releasing its GPU memory);
    /// a backend may override to also drop native objects it holds.
    virtual void on_expired() const;

    raw_buffer_handle _storage;
    isize _size_in_bytes = 0;
    isize _build_scratch_size_in_bytes = 0;
    isize _update_scratch_size_in_bytes = 0;
    accel_build_flags _build_flags = accel_build_flags::none;
    int _geometry_count = 0;
    mutable cc::vector<cc::unique_function<void()>> _finalizers; // mutable: add_finalizer is const (a lifetime hook)
    mutable std::atomic<bool> _expired{false};                   // mutable: expire() is a const lifetime hook
};

/// A top-level acceleration structure: an opaque index over a set of instances, each placing a blas with a
/// transform. A ray tracer traces against a tlas. Vocabulary type, held via tlas_handle. Abstract; storage +
/// stats here. Built through cmd.raytracing.build_tlas; the returned handle is persistent (valid across
/// epochs). A tlas keeps every referenced blas alive.
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
    mutable std::atomic<bool> _expired{false};
};
} // namespace sg
