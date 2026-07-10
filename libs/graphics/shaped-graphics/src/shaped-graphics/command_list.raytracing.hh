#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/acceleration_structure.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Ray-tracing recording facade for a command list, reached as `cmd.raytracing`: build acceleration
/// structures (and, later, trace rays). Building sizes + allocates the result buffer from a prebuild query,
/// records the GPU build with transient scratch, and returns a **persistent** handle (valid across epochs).
///
/// A thin facade over its owning command list: it forwards each op to the list's backend impl.
class command_list_raytracing_scope
{
public:
    /// Whether this backend/device supports ray tracing. When false, the build_* calls are unavailable —
    /// query this before building (e.g. to gate a feature or skip a test). dx12 answers from the device's
    /// raytracing tier; a backend without RT returns false.
    [[nodiscard]] bool is_supported() const;

    /// Build a triangle-geometry BLAS. All input buffers must carry buffer_usage::accel_structure_build_input;
    /// vertices are float3. Non-indexed geometries need `vertex_count % 3 == 0`; indexed ones `index_count %
    /// 3 == 0`. `fast_trace` / `fast_build` are mutually exclusive. Throws sg::allocation_exception if the
    /// result buffer can't be allocated. Requires is_supported().
    [[nodiscard]] blas_handle build_blas(cc::span<blas_triangles const> geometries,
                                         accel_build_flags flags = accel_build_flags::fast_trace);

    /// Build a procedural (AABB) BLAS. Same contract as the triangle overload; a BLAS is triangles or AABBs,
    /// never both.
    [[nodiscard]] blas_handle build_blas(cc::span<blas_aabbs const> geometries,
                                         accel_build_flags flags = accel_build_flags::fast_trace);

    /// Build a TLAS over `instances`. Each instance's `blas` must be non-null and already built; the TLAS
    /// holds every referenced blas_handle alive. `instance_id` / `hit_group_offset` are 24-bit (assert on
    /// overflow). Throws sg::allocation_exception if the result buffer can't be allocated. Requires
    /// is_supported().
    [[nodiscard]] tlas_handle build_tlas(cc::span<tlas_instance const> instances,
                                         accel_build_flags flags = accel_build_flags::fast_trace);

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_raytracing_scope(command_list_raytracing_scope const&) = delete;
    command_list_raytracing_scope(command_list_raytracing_scope&&) = delete;
    command_list_raytracing_scope& operator=(command_list_raytracing_scope const&) = delete;
    command_list_raytracing_scope& operator=(command_list_raytracing_scope&&) = delete;

private:
    // Only a command list constructs its own scope; the scope in turn reaches the list's protected backend
    // virtuals (mutual friendship).
    friend class command_list;
    explicit command_list_raytracing_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
} // namespace sg
