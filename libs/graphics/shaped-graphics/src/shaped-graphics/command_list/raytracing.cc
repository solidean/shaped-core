#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/impl/hit_record_check.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

namespace sg
{
void command_list::bind_raytracing_pipeline(raytracing_pipeline const& pipeline)
{
    if (auto const* layout = pipeline.footprint().layout(); layout == nullptr || layout != _raytracing_layout)
    {
        _raytracing_layout = layout;
        for (auto& g : _raytracing_groups)
            g = nullptr;
    }
    raytracing_bind_pipeline(pipeline);
}

void command_list::bind_raytracing_group(int group_index, binding_group const& group)
{
    CC_ASSERT(group_index >= 0 && group_index < max_binding_groups, "binding-group slot out of range");
    _raytracing_groups[group_index] = &group;
    raytracing_bind_group(group_index, group);
}

void command_list::dispatch_rays(raytracing_shader_table const& table, raygen_index raygen, int width, int height, int depth)
{
    check_cluster_pools(_raytracing_groups, "dispatch_rays");
    if (context().hit_record_checks() && check_hit_records(table))
    {
        CC_LOG_ERROR("dispatch_rays skipped a dispatch whose hit records reach past the shader table's end");
        return;
    }
    raytracing_dispatch_rays(table, raygen, width, height, depth);
}

void command_list::check_cluster_pools(cc::span<binding_group const* const> groups, cc::string_view what) const
{
    if (!context().hit_record_checks())
        return;
    for (auto const* group : groups)
        if (group != nullptr)
            for (auto const& tlas : impl::tlases_of(*group))
                CC_ASSERTF(!impl::reaches_rebuilt_pool(*tlas),
                           "{}: a bound tlas reaches a cluster BLAS whose pool was rebuilt since the tlas was built, "
                           "so "
                           "the clusters it lists are gone",
                           what);
}

bool command_list::check_hit_records(raytracing_shader_table const& table)
{
    auto past_table_end = false;
    for (auto const* group : _raytracing_groups)
    {
        if (group == nullptr)
            continue;
        for (auto const& tlas : impl::tlases_of(*group))
        {
            auto const* checked = static_cast<checked_trace const*>(nullptr);
            for (auto const& c : _checked_traces)
                if (c.table.get() == &table && c.tlas == tlas)
                    checked = &c;
            if (checked != nullptr)
            {
                past_table_end = past_table_end || checked->past_table_end;
                continue;
            }

            auto const mismatches = impl::find_hit_record_mismatches(table, *tlas);
            auto text = cc::string();
            auto reaches_past_end = false;
            for (auto const& m : mismatches)
            {
                text += cc::format("\n  {}", m.message);
                reaches_past_end = reaches_past_end || m.past_table_end;
            }
            // A table no handle owns keys nothing, so it is checked at every dispatch rather than skipped wrongly.
            _checked_traces.push_back(
                {.table = table.weak_from_this().lock(), .tlas = tlas, .past_table_end = reaches_past_end});
            past_table_end = past_table_end || reaches_past_end;
            // Logged rather than asserted: the hit groups come from shaders that hot reload can change under a running program.
            if (!mismatches.empty())
                CC_LOG_ERROR("dispatch_rays traces a tlas whose instances reach hit records of the wrong kind or out "
                             "of "
                             "the shader table, which is undefined behavior:{}",
                             text);
        }
    }
    return past_table_end;
}

bool command_list_raytracing_scope::is_supported() const
{
    return _cmd.raytracing_is_supported();
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_triangles const> geometries,
                                                      accel_build_flags flags,
                                                      int hit_record_stride)
{
    CC_ASSERT(hit_record_stride >= 1, "build_blas: hit_record_stride must be >= 1");
    auto* const pool = flags.has(accel_build_flag::software_trace) ? _cmd.context().software_pool() : nullptr;
    // a native trace that sees no motion leaves it to the software walk, whose structure only a flagged build writes
    auto const has_native_trace
        = _cmd.context().implementation_of(feature::ray_query) == feature_implementation::native
       && _cmd.context().implementation_of(feature::ray_tracing_motion_blur) != feature_implementation::native;
    auto micromaps = cc::vector<micromap_handle>();
    for (auto const& g : geometries)
    {
        CC_ASSERT(g.later_poses.empty() || !has_native_trace || pool != nullptr,
                  "build_blas: a geometry moves, which only the software walk sees, so its build takes "
                  "accel_build_flag::software_trace");
        CC_ASSERT(g.later_poses.empty() || _cmd.context().supports(feature::ray_tracing_motion_blur),
                  "build_blas: a geometry moves, and this context has no sg::feature::ray_tracing_motion_blur");
        CC_ASSERT(g.later_poses.size() <= 1, "build_blas: a geometry takes one later pose, its vertices at time 1");
        for (auto const& pose : g.later_poses)
            CC_ASSERT(pose.vertices != nullptr, "build_blas: a geometry's later pose names no vertices");
        if (g.micromap == nullptr)
            continue;
        CC_ASSERT(_cmd.context().supports(feature::opacity_micromap),
                  "build_blas: a geometry names a micromap, and this context has no sg::feature::opacity_micromap");
        CC_ASSERT(!g.micromap->is_expired(), "build_blas: a geometry names an expired micromap");
        CC_ASSERT(g.micromap_indices != nullptr, "build_blas: a geometry with a micromap needs its micromap_indices");
        micromaps.push_back(g.micromap);
    }
    _cmd._stats.add(stat::blas_builds);
    auto blas = _cmd.raytracing_build_blas_triangles(geometries, flags, hit_record_stride);
    if (blas != nullptr)
    {
        if (pool != nullptr)
            impl::set_software_region(*blas, impl::build_software_blas(*pool, geometries));
        impl::set_build_record(*blas, blas_geometry::triangles, hit_record_stride);
        if (!micromaps.empty())
            impl::set_micromaps(*blas, cc::move(micromaps));
    }
    return blas;
}

micromap_handle command_list_raytracing_scope::build_micromap(micromap_triangles const& triangles)
{
    CC_ASSERT(_cmd.context().supports(feature::opacity_micromap),
              "build_micromap: this context has no sg::feature::opacity_micromap (check ctx.supports first)");
    CC_ASSERT(triangles.states != nullptr && triangles.descriptors != nullptr, "build_micromap: the states and the "
                                                                               "descriptors are both required");
    auto maps = isize(0);
    for (auto const& u : triangles.usage)
        maps += isize(u.count);
    CC_ASSERT(maps > 0, "build_micromap: the usage names no map");
    return _cmd.raytracing_build_micromap(triangles);
}

blas_handle command_list_raytracing_scope::build_blas(cc::span<blas_aabbs const> geometries,
                                                      accel_build_flags flags,
                                                      int hit_record_stride)
{
    CC_ASSERT(hit_record_stride >= 1, "build_blas: hit_record_stride must be >= 1");
    _cmd._stats.add(stat::blas_builds);
    auto blas = _cmd.raytracing_build_blas_aabbs(geometries, flags, hit_record_stride);
    if (blas != nullptr)
    {
        if (auto* const pool = flags.has(accel_build_flag::software_trace) ? _cmd.context().software_pool() : nullptr)
            impl::set_software_region(*blas, impl::build_software_blas(*pool, geometries));
        impl::set_build_record(*blas, blas_geometry::aabbs, hit_record_stride);
    }
    return blas;
}

tlas_handle command_list_raytracing_scope::build_tlas(cc::span<tlas_instance const> instances, accel_build_flags flags)
{
    _cmd._stats.add(stat::tlas_builds);
    auto* const pool = flags.has(accel_build_flag::software_trace) ? _cmd.context().software_pool() : nullptr;
    // a native trace that sees no motion leaves it to the software walk, whose structure only a flagged build writes
    auto const has_native_trace
        = _cmd.context().implementation_of(feature::ray_query) == feature_implementation::native
       && _cmd.context().implementation_of(feature::ray_tracing_motion_blur) != feature_implementation::native;
    for (auto const& instance : instances)
    {
        CC_ASSERT(instance.blas == nullptr || !instance.blas->is_stale(), "build_tlas: an instance's cluster BLAS is "
                                                                          "stale: its pool was rebuilt since");
        CC_ASSERT(!instance.transform_at_end.has_value() || _cmd.context().supports(feature::ray_tracing_motion_blur),
                  "build_tlas: an instance moves, and this context has no sg::feature::ray_tracing_motion_blur");
        CC_ASSERT(!instance.transform_at_end.has_value() || !has_native_trace || pool != nullptr,
                  "build_tlas: an instance moves, which only the software walk sees, so the build takes "
                  "accel_build_flag::software_trace");
    }
    auto tlas = _cmd.raytracing_build_tlas(instances, flags);
    if (tlas != nullptr && pool != nullptr)
        impl::set_software_region(*tlas, impl::build_software_tlas(*pool, instances));

    // Only what the hit-record check at dispatch_rays reads, and only where it runs.
    if (tlas != nullptr && _cmd.context().hit_record_checks())
    {
        auto records = cc::vector<impl::tlas_instance_record>();
        records.reserve(instances.size());
        for (auto const& inst : instances)
            records.push_back({.blas = inst.blas, .hit_group_offset = inst.hit_group_offset, .mask = inst.mask});
        impl::set_instance_records(*tlas, cc::move(records));
    }
    return tlas;
}

void command_list::raytracing_update_partitioned_tlas(partitioned_tlas const&,
                                                      ptlas_operations const&,
                                                      bool,
                                                      cc::vector<blas_handle>&&)
{
    CC_ASSERT(false, "a backend that reports sg::feature::partitioned_tlas overrides "
                     "raytracing_update_partitioned_tlas");
}

void command_list::raytracing_update_partitioned_tlas_indirect(partitioned_tlas const&,
                                                               ptlas_indirect_operations const&,
                                                               bool,
                                                               cc::vector<blas_table_handle>&&)
{
    CC_ASSERT(false, "a backend that reports sg::feature::partitioned_tlas overrides "
                     "raytracing_update_partitioned_tlas_indirect");
}

cluster_set_handle command_list::raytracing_build_clusters(cc::span<cluster_triangles const>, accel_build_flags)
{
    CC_ASSERT(false, "a backend that reports sg::feature::clustered_geometry overrides raytracing_build_clusters");
    return nullptr;
}

void command_list::raytracing_build_clusters_indirect(cluster_pool const&, cluster_indirect_build const&)
{
    CC_ASSERT(false, "a backend that reports sg::feature::clustered_geometry overrides "
                     "raytracing_build_clusters_indirect");
}

cc::vector<blas_handle> command_list::raytracing_build_cluster_blas(cluster_set_handle const&,
                                                                    cc::span<cluster_range const>,
                                                                    accel_build_flags,
                                                                    int)
{
    CC_ASSERT(false, "a backend that reports sg::feature::clustered_geometry overrides raytracing_build_cluster_blas");
    return {};
}

namespace
{
/// Checks one kind of record against the buffers it names, which the build then reads.
void validate_indirect_records(indirect_records const& r, isize stride, char const* what)
{
    if (r.max_count == 0)
        return;
    CC_ASSERTF(r.max_count > 0, "update_indirect: {} has a negative max_count", what);
    CC_ASSERTF(r.records != nullptr && r.count != nullptr, "update_indirect: {} needs a records and a count buffer",
               what);
    CC_ASSERTF(r.offset_in_bytes >= 0 && r.offset_in_bytes % 16 == 0,
               "update_indirect: the records of {} start at {}, which is no multiple of 16", what, r.offset_in_bytes);
    CC_ASSERTF(r.offset_in_bytes + isize(r.max_count) * stride <= r.records->size_in_bytes(),
               "update_indirect: {} records of {}, of {} bytes from {}, overrun their buffer of {}", r.max_count, what,
               stride, r.offset_in_bytes, r.records->size_in_bytes());
    CC_ASSERTF(r.count_offset_in_bytes >= 0 && r.count_offset_in_bytes % 4 == 0
                   && r.count_offset_in_bytes + 4 <= r.count->size_in_bytes(),
               "update_indirect: the count of {} at {} is no aligned u32 of its buffer", what, r.count_offset_in_bytes);
    CC_ASSERTF(r.count->usage().has(buffer_usage::copy_src), "update_indirect: the count buffer of {} needs copy_src",
               what);
}
} // namespace

void command_list_raytracing_scope::update(partitioned_tlas const& ptlas, ptlas_operations const& ops)
{
    CC_ASSERT(_cmd.context().supports(feature::partitioned_tlas), "update: this context has no "
                                                                  "sg::feature::partitioned_tlas");
    CC_ASSERT(!ptlas.is_expired(), "update: the partitioned tlas is expired");
    _cmd._stats.add(stat::tlas_builds);
    auto released = impl::apply_to_slots(ptlas, ops);
    if (_cmd.context().hit_record_checks() && !ptlas.is_gpu_written())
        impl::set_instance_records(ptlas, impl::slot_records(ptlas));
    _cmd.raytracing_update_partitioned_tlas(ptlas, ops, impl::mark_updated(ptlas), cc::move(released));
}

void command_list_raytracing_scope::update_indirect(partitioned_tlas const& ptlas, ptlas_indirect_operations const& ops)
{
    CC_ASSERT(_cmd.context().supports(feature::partitioned_tlas), "update_indirect: this context has no "
                                                                  "sg::feature::partitioned_tlas");
    CC_ASSERT(!ptlas.is_expired(), "update_indirect: the partitioned tlas is expired");
    validate_indirect_records(ops.writes, isize(sizeof(gpu_ptlas_write)), "the writes");
    validate_indirect_records(ops.updates, isize(sizeof(gpu_ptlas_update)), "the updates");
    validate_indirect_records(ops.translations, isize(sizeof(gpu_ptlas_translation)), "the translations");
    CC_ASSERT(ops.translations.max_count == 0 || ptlas.description().allows_partition_translation,
              "update_indirect: a translation needs partitioned_tlas_description::allows_partition_translation");
    for (auto const& table : ops.tables)
        CC_ASSERT(table != nullptr, "update_indirect: a table is null");
    _cmd._stats.add(stat::tlas_builds);
    auto released = impl::hold_tables(ptlas, ops.tables);
    // what the records hold is the GPU's to know, so no check can follow them
    if (impl::mark_gpu_written(ptlas) && _cmd.context().hit_record_checks())
        CC_LOG_INFO("a partitioned tlas was updated from GPU-written records, so hit_record_checks no longer checks "
                    "its instances");
    _cmd.raytracing_update_partitioned_tlas_indirect(ptlas, ops, impl::mark_updated(ptlas), cc::move(released));
}

cluster_set_handle command_list_raytracing_scope::build_clusters(cc::span<cluster_triangles const> clusters,
                                                                 accel_build_flags flags)
{
    CC_ASSERT(_cmd.context().supports(feature::clustered_geometry), "build_clusters: this context has no "
                                                                    "sg::feature::clustered_geometry");
    CC_ASSERT(!clusters.empty(), "build_clusters: a set holds at least one cluster");
    CC_ASSERT(flags.without(accel_build_flag::fast_trace | accel_build_flag::fast_build).is_empty(),
              "build_clusters: only fast_trace or fast_build apply");
    CC_ASSERT(!flags.has_all(accel_build_flag::fast_trace | accel_build_flag::fast_build),
              "build_clusters: fast_trace and fast_build are mutually exclusive");
    for (auto i = isize(0); i < clusters.size(); ++i)
    {
        auto const& c = clusters[i];
        CC_ASSERTF(c.vertices != nullptr && c.indices != nullptr,
                   "build_clusters: cluster {} needs vertices and "
                   "indices",
                   i);
        CC_ASSERTF(c.vertex_count >= 1 && c.vertex_count <= max_cluster_vertices,
                   "build_clusters: cluster {} has {} vertices, of 1 to {}", i, c.vertex_count, max_cluster_vertices);
        CC_ASSERTF(c.triangle_count >= 1 && c.triangle_count <= max_cluster_triangles,
                   "build_clusters: cluster {} has {} triangles, of 1 to {}", i, c.triangle_count, max_cluster_triangles);
        CC_ASSERTF(c.geometry_index >= 0 && c.geometry_index < (1 << 24),
                   "build_clusters: cluster {}'s geometry index must fit in 24 bits", i);
        CC_ASSERTF(c.vertices->usage().has(buffer_usage::accel_structure_build_input)
                       && c.indices->usage().has(buffer_usage::accel_structure_build_input),
                   "build_clusters: cluster {}'s buffers need buffer_usage::accel_structure_build_input", i);
        auto const index_size = c.index_type == index_format::uint16 ? isize(2) : isize(4);
        CC_ASSERTF(
            c.index_offset_in_bytes >= 0
                && c.index_offset_in_bytes + isize(c.triangle_count) * 3 * index_size <= c.indices->size_in_bytes(),
            "build_clusters: cluster {}'s indices overrun their buffer", i);
        CC_ASSERTF(c.vertex_offset_in_bytes >= 0 && c.vertex_stride_in_bytes >= isize(sizeof(float) * 3)
                       && c.vertex_offset_in_bytes + isize(c.vertex_count - 1) * c.vertex_stride_in_bytes
                                  + isize(sizeof(float) * 3)
                              <= c.vertices->size_in_bytes(),
                   "build_clusters: cluster {}'s vertices overrun their buffer", i);
    }
    _cmd._stats.add(stat::blas_builds);
    return _cmd.raytracing_build_clusters(clusters, flags);
}

void command_list_raytracing_scope::build_clusters_indirect(cluster_pool const& pool, cluster_indirect_build const& build)
{
    CC_ASSERT(_cmd.context().supports(feature::clustered_geometry), "build_clusters_indirect: this context has no "
                                                                    "sg::feature::clustered_geometry");
    CC_ASSERT(build.clusters.max_count >= 1 && build.clusters.max_count <= pool.cluster_count(),
              "build_clusters_indirect: max_count must be 1 to the pool's capacity");
    // the stride is the backend's, so the records are checked against the largest, which every buffer of them sizes for
    validate_indirect_records(build.clusters, isize(sizeof(gpu_clas_triangles)), "the clusters");
    CC_ASSERT(build.clusters.records->usage().has(buffer_usage::accel_structure_build_input)
                  && build.clusters.count->usage().has(buffer_usage::accel_structure_build_input),
              "build_clusters_indirect: the records and their count need buffer_usage::accel_structure_build_input");
    for (auto const& table : build.tables)
        CC_ASSERT(table != nullptr, "build_clusters_indirect: a table is null");
    _cmd._stats.add(stat::blas_builds);
    (void)impl::bump_generation(pool);
    _cmd.raytracing_build_clusters_indirect(pool, build);
}

cc::vector<blas_handle> command_list_raytracing_scope::build_cluster_blas(cluster_set_handle const& clusters,
                                                                          cc::span<cluster_range const> ranges,
                                                                          accel_build_flags flags,
                                                                          int hit_record_stride)
{
    CC_ASSERT(_cmd.context().supports(feature::clustered_geometry), "build_cluster_blas: this context has no "
                                                                    "sg::feature::clustered_geometry");
    CC_ASSERT(clusters != nullptr, "build_cluster_blas: the cluster set is null");
    CC_ASSERT(!ranges.empty(), "build_cluster_blas: at least one range");
    CC_ASSERT(hit_record_stride >= 1, "build_cluster_blas: hit_record_stride must be >= 1");
    CC_ASSERT(flags.without(accel_build_flag::fast_trace | accel_build_flag::fast_build).is_empty(),
              "build_cluster_blas: only fast_trace or fast_build apply");
    CC_ASSERT(!flags.has_all(accel_build_flag::fast_trace | accel_build_flag::fast_build),
              "build_cluster_blas: fast_trace and fast_build are mutually exclusive");
    for (auto const& r : ranges)
        CC_ASSERTF(r.count >= 1 && r.first >= 0 && r.first + r.count <= clusters->cluster_count(),
                   "build_cluster_blas: clusters {} to {} are not within a set of {}", r.first, r.first + r.count,
                   clusters->cluster_count());
    _cmd._stats.add(stat::blas_builds);
    auto blases = _cmd.raytracing_build_cluster_blas(clusters, ranges, flags, hit_record_stride);
    for (auto const& blas : blases)
    {
        impl::set_build_record(*blas, blas_geometry::triangles, hit_record_stride);
        impl::set_clusters(*blas, clusters);
    }
    return blases;
}

void command_list_raytracing_scope::bind_pipeline(raytracing_pipeline const& pipeline)
{
    _cmd.bind_raytracing_pipeline(pipeline);
}

void command_list_raytracing_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_raytracing_group(group_index, group);
}

void command_list_raytracing_scope::dispatch_rays(raytracing_shader_table const& table,
                                                  raygen_index raygen,
                                                  int width,
                                                  int height,
                                                  int depth)
{
    _cmd._stats.add(stat::ray_dispatches);
    _cmd.dispatch_rays(table, raygen, width, height, depth);
}

array_access_declaration command_list_raytracing_scope::declare_array_buffer_access(
    buffer_array_binding array,
    cc::span<array_buffer_access const> elements)
{
    // Ray tracing binds through the compute path (same root signature, same bind point), so the compute seam serves both.
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_buffer_access(array.name, elements), false);
}

array_access_declaration command_list_raytracing_scope::declare_array_buffer_access(
    cc::string_view binding_name,
    cc::span<array_buffer_access const> elements)
{
    return declare_array_buffer_access(_cmd.named_buffer_array(binding_name), elements);
}

array_access_declaration command_list_raytracing_scope::declare_array_texture_access(
    texture_array_binding array,
    cc::span<array_texture_access const> elements)
{
    return _cmd.adopt_array_declaration(_cmd.compute_declare_array_texture_access(array.name, elements), false);
}

array_access_declaration command_list_raytracing_scope::declare_array_texture_access(
    cc::string_view binding_name,
    cc::span<array_texture_access const> elements)
{
    return declare_array_texture_access(_cmd.named_texture_array(binding_name), elements);
}
} // namespace sg
