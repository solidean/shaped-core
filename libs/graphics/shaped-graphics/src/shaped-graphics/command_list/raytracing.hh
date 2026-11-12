#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding_group.hh> // sg::declared_binding_group, for bind<G>
#include <shaped-graphics/command_list/array_access_declaration.hh>
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
    /// A geometry may name an opacity micromap only where the context has `sg::feature::opacity_micromap`, and the BLAS then holds it.
    ///
    /// `hit_record_stride` is the number of shader-table records one geometry of this BLAS takes: the ray count of the pipelines that trace it, and must be >= 1.
    /// Metal bakes it into the structure, setting geometry g's intersection-function-table offset to `g * hit_record_stride`.
    /// dx12 and vulkan take the multiplier per trace instead and ignore it.
    [[nodiscard]] blas_handle build_blas(cc::span<blas_triangles const> geometries,
                                         accel_build_flags flags = accel_build_flag::fast_trace,
                                         int hit_record_stride = 1);

    /// Build an opacity micromap, which a later `build_blas` names in a `blas_triangles`.
    /// Requires `sg::feature::opacity_micromap`; `triangles.usage` must name at least one map, and its levels the device's maximum at most.
    /// Throws sg::allocation_exception if the result buffer cannot be allocated.
    [[nodiscard]] micromap_handle build_micromap(micromap_triangles const& triangles);

    /// Build a procedural (AABB) BLAS.
    /// Same contract as the triangle overload; a BLAS is triangles or AABBs, never both.
    [[nodiscard]] blas_handle build_blas(cc::span<blas_aabbs const> geometries,
                                         accel_build_flags flags = accel_build_flag::fast_trace,
                                         int hit_record_stride = 1);

    /// Build a TLAS over `instances`.
    /// Each instance's `blas` must be non-null and already built, and the TLAS holds every referenced blas_handle alive.
    /// `instance_id` / `hit_group_offset` are 24-bit, and assert on overflow.
    /// Under `context::hit_record_checks` the TLAS also keeps each instance's BLAS, offset and mask, which is what dispatch_rays checks.
    /// Throws sg::allocation_exception if the result buffer cannot be allocated.
    /// Requires is_supported().
    [[nodiscard]] tlas_handle build_tlas(cc::span<tlas_instance const> instances,
                                         accel_build_flags flags = accel_build_flag::fast_trace);

    /// Applies `ops` to `ptlas` in place, rebuilding only the partitions they touch; its first update builds it.
    /// A slot is named once at most across the writes, updates and clears, and each blas they name must be built.
    /// The partitioned_tlas holds every blas its slots name, and this list keeps the ones it replaces until it has run.
    /// Under `context::hit_record_checks` it also keeps each slot's blas, offset and mask, which is what dispatch_rays checks.
    /// Requires `sg::feature::partitioned_tlas`.
    void update(partitioned_tlas const& ptlas, ptlas_operations const& ops);

    /// Applies records a compute pass wrote to `ptlas`, as `update` applies its operations: the GPU-written tier.
    /// sg writes the operation array and copies each count into it, so a shader writes the records and bumps a count.
    /// The records are SGL's `accel` module's, and each count must not exceed its `max_count`.
    /// `ops.tables` are every table a record may name after this update, which the structure then holds in place of
    /// the earlier update's; a record naming a BLAS of no such table is a caller bug sg cannot see.
    /// No check follows the records, so under `context::hit_record_checks` the structure stops being checked.
    /// Requires `sg::feature::partitioned_tlas`.
    void update_indirect(partitioned_tlas const& ptlas, ptlas_indirect_operations const& ops);

    /// Builds `clusters` into one immutable set, cluster k at position k, which `build_cluster_blas` then lists.
    /// Each must be indexed, with 1 to `max_cluster_triangles` triangles over 1 to `max_cluster_vertices` vertices.
    /// `fast_trace` / `fast_build` are mutually exclusive, and nothing else applies.
    /// Requires `sg::feature::clustered_geometry`; throws sg::allocation_exception if the set cannot be allocated.
    [[nodiscard]] cluster_set_handle build_clusters(cc::span<cluster_triangles const> clusters,
                                                    accel_build_flags flags = accel_build_flag::fast_trace);

    /// Replaces every cluster of `pool` with those a compute pass wrote as `accel.clas_triangles` records, in place.
    /// The records and their count carry `buffer_usage::accel_structure_build_input`, which the build reads them as.
    /// The count must not exceed `build.clusters.max_count`, nor that the pool's capacity, and every record's buffers
    /// must be addresses of `build.tables`, whose index format is the pool's.
    /// Every cluster BLAS built from the pool before is stale from then on.
    /// Requires `sg::feature::clustered_geometry`.
    void build_clusters_indirect(cluster_pool const& pool, cluster_indirect_build const& build);

    /// Builds one BLAS per range, each listing those clusters of `clusters`, and each holding the set.
    /// A range is non-empty and within the set, and ranges may overlap.
    /// A cluster BLAS has `max_geometry_index() + 1` geometries, and `hit_record_stride` is build_blas's.
    /// Requires `sg::feature::clustered_geometry`; throws sg::allocation_exception if a BLAS cannot be allocated.
    [[nodiscard]] cc::vector<blas_handle> build_cluster_blas(cluster_set_handle const& clusters,
                                                             cc::span<cluster_range const> ranges,
                                                             accel_build_flags flags = accel_build_flag::fast_trace,
                                                             int hit_record_stride = 1);

    /// Binds a raytracing_pipeline for the following bind_group / dispatch_rays.
    /// Requires is_supported().
    void bind_pipeline(raytracing_pipeline const& pipeline);

    /// Binds `group` at slot `group_index` of the bound pipeline's layout, validated against it.
    /// Ray tracing binds through the pipeline's global root signature, like compute.
    void bind_group(int group_index, binding_group const& group);

    /// Binds `group` at the index the generated group `G` declares, so no call site writes the number.
    /// The backend still checks that index against the layout the group was built for — this only saves
    /// spelling it.
    template <declared_binding_group G>
    void bind(binding_group const& group)
    {
        bind_group(G::group_index, group);
    }

    /// Traces a `width` x `height` x `depth` grid of rays, launching the raygen shader at `raygen` in `table`.
    /// Each dimension must be >= 1, and their product <= 2^30.
    /// Requires a bound pipeline, and `table` must have been built for it.
    ///
    /// Under `context::hit_record_checks`, every instance of every bound tlas is checked against the hit records it reaches.
    /// Each must exist, and be procedural exactly when the instance's BLAS holds AABBs.
    /// A mismatch logs an error, once per table and tlas in a list, rather than asserting, since hot reload can change a hit group under a running program.
    /// **A dispatch reaching a record past the table's end is skipped**, every time in that list, since metal can fault the GPU on it.
    /// One reaching only records of the wrong kind still runs.
    /// Only a tlas built, and a group or staging group created, while the checks were on is seen.
    void dispatch_rays(raytracing_shader_table const& table, raygen_index raygen, int width, int height = 1, int depth = 1);

    /// Declares per-element access for a *buffer* array / bindless binding, for every trace while the result lives.
    /// The compute scope's contract at the raytracing stage, and the same bind point: a declaration made here is in force for dispatches too.
    /// An array the pipeline's code indexes with no live declaration logs and is covered whole, and an empty `elements` declares it unused.
    /// `array` is what the shader package generates for the binding, and the by-name overload checks the name as the compute one does.
    [[nodiscard]] array_access_declaration declare_array_buffer_access(buffer_array_binding array,
                                                                       cc::span<array_buffer_access const> elements);
    [[nodiscard]] array_access_declaration declare_array_buffer_access(cc::string_view binding_name,
                                                                       cc::span<array_buffer_access const> elements);

    /// The texture form: each element also names the layout it must be in.
    [[nodiscard]] array_access_declaration declare_array_texture_access(texture_array_binding array,
                                                                        cc::span<array_texture_access const> elements);
    [[nodiscard]] array_access_declaration declare_array_texture_access(cc::string_view binding_name,
                                                                        cc::span<array_texture_access const> elements);

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
