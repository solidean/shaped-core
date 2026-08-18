#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>

/// Ray-tracing recording facade for a command list, reached as `cmd.raytracing`: build acceleration structures and trace rays.
/// A build sizes and allocates the result buffer from a prebuild query, records the GPU build with transient scratch, and returns a **persistent** handle valid across epochs.
/// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.
/// libs/graphics/shaped-graphics/docs/concepts/raytracing-pipeline.md covers the trace path.
class sg::command_list_raytracing_scope
{
public:
    /// Whether this backend/device supports ray tracing.
    /// When false the build_* calls are unavailable, so query this before building — to gate a feature, or to skip a test.
    /// dx12 answers from the device's raytracing tier; a backend without RT returns false.
    [[nodiscard]] bool is_supported() const;

    /// Build a triangle-geometry BLAS.
    /// All input buffers must carry buffer_usage::accel_structure_build_input, and vertices are float3.
    /// Non-indexed geometries need `vertex_count % 3 == 0`, indexed ones `index_count % 3 == 0`, and `fast_trace` / `fast_build` are mutually exclusive.
    /// Throws sg::allocation_exception if the result buffer cannot be allocated.
    /// Requires is_supported().
    [[nodiscard]] blas_handle build_blas(cc::span<blas_triangles const> geometries,
                                         accel_build_flags flags = accel_build_flag::fast_trace);

    /// Build a procedural (AABB) BLAS.
    /// Same contract as the triangle overload; a BLAS is triangles or AABBs, never both.
    [[nodiscard]] blas_handle build_blas(cc::span<blas_aabbs const> geometries,
                                         accel_build_flags flags = accel_build_flag::fast_trace);

    /// Build a TLAS over `instances`.
    /// Each instance's `blas` must be non-null and already built, and the TLAS holds every referenced blas_handle alive.
    /// `instance_id` / `hit_group_offset` are 24-bit, and assert on overflow.
    /// Throws sg::allocation_exception if the result buffer cannot be allocated.
    /// Requires is_supported().
    [[nodiscard]] tlas_handle build_tlas(cc::span<tlas_instance const> instances,
                                         accel_build_flags flags = accel_build_flag::fast_trace);

    /// Binds a raytracing_pipeline for the following bind_group / dispatch_rays.
    /// Requires is_supported().
    void bind_pipeline(raytracing_pipeline const& pipeline);

    /// Binds `group` at slot `set` of the bound pipeline's layout, validated against it.
    /// Ray tracing binds through the pipeline's global root signature, like compute.
    void bind_group(int set, binding_group const& group);

    /// Traces a `width` x `height` x `depth` grid of rays, launching the raygen shader at `raygen` in `table`.
    /// Each dimension must be >= 1, and their product <= 2^30.
    /// Requires a bound pipeline, and `table` must have been built for it.
    void dispatch_rays(raytracing_shader_table const& table, raygen_index raygen, int width, int height = 1, int depth = 1);

    // Pinned to its owning command list: neither copyable nor movable.
    command_list_raytracing_scope(command_list_raytracing_scope const&) = delete;
    command_list_raytracing_scope(command_list_raytracing_scope&&) = delete;
    command_list_raytracing_scope& operator=(command_list_raytracing_scope const&) = delete;
    command_list_raytracing_scope& operator=(command_list_raytracing_scope&&) = delete;

private:
    // Only a command list constructs its own scope, and the scope reaches the list's protected backend virtuals — mutual friendship.
    friend class command_list;
    explicit command_list_raytracing_scope(command_list& cmd) : _cmd(cmd) {}

    command_list& _cmd;
};
