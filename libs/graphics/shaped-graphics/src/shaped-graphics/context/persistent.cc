#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/impl/portability.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/capabilities.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/persistent.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raytracing/blas_table.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/partitioned_tlas.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sg
{
// buffers

raw_buffer_handle context_persistent_scope::create_raw_buffer(isize size_in_bytes,
                                                              buffer_usages usage,
                                                              allocation_info const& alloc)
{
    auto r = try_create_raw_buffer(size_in_bytes, usage, alloc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("persistent buffer allocation failed", size_in_bytes, r.error());
}

cc::result<raw_buffer_handle> context_persistent_scope::try_create_raw_buffer(isize size_in_bytes,
                                                                              buffer_usages usage,
                                                                              allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    _ctx._stats.add(stat::buffers_created);
    return _ctx.try_create_raw_buffer(size_in_bytes, usage, alloc);
}

raw_buffer_handle context_persistent_scope::create_raw_buffer_from_pin(cc::pinned_data<byte const> bytes,
                                                                       buffer_usages usage,
                                                                       allocation_info const& alloc)
{
    auto buffer = create_raw_buffer(bytes.size(), usage | buffer_usage::copy_dst, alloc);
    _ctx.upload.bytes_to_buffer(buffer, cc::move(bytes));
    return buffer;
}

// textures

raw_texture_handle context_persistent_scope::create_raw_texture(texture_description const& desc,
                                                                allocation_info const& alloc)
{
    auto r = try_create_raw_texture(desc, alloc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("persistent texture allocation failed", 0, r.error());
}

cc::result<raw_texture_handle> context_persistent_scope::try_create_raw_texture(texture_description const& desc,
                                                                                allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == lifetime_scope::persistent, "persistent scope requires a persistent allocation");
    if (auto unsupported = impl::find_unsupported_texture(_ctx.supports(feature::extended_image_formats), desc);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    if (is_compressed_format(desc.format) && !_ctx.supports(feature::texture_compression_bc))
        return cc::error("a block-compressed texture needs sg::feature::texture_compression_bc, which this device "
                         "lacks");
    if (auto error = desc.unaligned_block_error(_ctx.supports(feature::unaligned_block_compression)); !error.empty())
        return cc::error(cc::move(error));
    _ctx._stats.add(stat::textures_created);
    return _ctx.try_create_raw_texture(desc, alloc);
}

// memory heaps

memory_heap_handle context_persistent_scope::create_memory_heap(isize size_in_bytes, memory_heap_contents contents)
{
    auto r = try_create_memory_heap(size_in_bytes, contents);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("memory heap allocation failed", size_in_bytes, r.error());
}

cc::result<memory_heap_handle> context_persistent_scope::try_create_memory_heap(isize size_in_bytes,
                                                                                memory_heap_contents contents)
{
    return _ctx.try_create_memory_heap(size_in_bytes, contents);
}

partitioned_tlas_handle context_persistent_scope::create_partitioned_tlas(partitioned_tlas_description const& desc)
{
    CC_ASSERT(_ctx.supports(feature::partitioned_tlas), "create_partitioned_tlas: this context has no "
                                                        "sg::feature::partitioned_tlas");
    CC_ASSERT(desc.instance_capacity > 0, "create_partitioned_tlas: instance_capacity must be > 0");
    CC_ASSERT(desc.partition_count >= 0 && desc.max_global_instances >= 0, "create_partitioned_tlas: partition_count "
                                                                           "and max_global_instances must be >= 0");
    CC_ASSERT(desc.partition_count == 0 || desc.max_instances_per_partition > 0, "create_partitioned_tlas: a partition "
                                                                                 "holds at least one instance");
    CC_ASSERT(desc.partition_count > 0 || desc.max_global_instances > 0, "create_partitioned_tlas: no partition can "
                                                                         "hold an instance");
    CC_ASSERT(desc.flags.without(accel_build_flag::fast_trace | accel_build_flag::fast_build).is_empty(),
              "create_partitioned_tlas: only fast_trace or fast_build apply");
    CC_ASSERT(!desc.flags.has_all(accel_build_flag::fast_trace | accel_build_flag::fast_build),
              "create_partitioned_tlas: fast_trace and fast_build are mutually exclusive");
    auto r = _ctx.try_create_partitioned_tlas(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("partitioned tlas allocation failed", 0, r.error());
}

blas_table_handle context_persistent_scope::create_blas_table(cc::span<blas_handle const> blases)
{
    CC_ASSERT(_ctx.supports(feature::partitioned_tlas), "create_blas_table: this context has no "
                                                        "sg::feature::partitioned_tlas");
    CC_ASSERT(!blases.empty(), "create_blas_table: a table lists at least one blas");
    auto addresses = cc::vector<gpu_blas_ref>();
    for (auto const& blas : blases)
    {
        CC_ASSERT(blas != nullptr && !blas->is_expired(), "create_blas_table: a blas is null or expired");
        addresses.push_back({.address = _ctx.blas_address_of(*blas)});
    }
    auto buffer = create_buffer_from_data(cc::span<gpu_blas_ref const>(addresses), buffer_usage::readonly_buffer);
    return std::make_shared<blas_table const>(cc::vector<blas_handle>::create_copy_of(blases), cc::move(buffer));
}

buffer_table_handle context_persistent_scope::create_buffer_table(cc::span<raw_buffer_handle const> buffers)
{
    CC_ASSERT(_ctx.supports(feature::clustered_geometry), "create_buffer_table: this context has no "
                                                          "sg::feature::clustered_geometry");
    CC_ASSERT(!buffers.empty(), "create_buffer_table: a table lists at least one buffer");
    auto addresses = cc::vector<gpu_buffer_ref>();
    for (auto const& b : buffers)
    {
        CC_ASSERT(b != nullptr && !b->is_expired(), "create_buffer_table: a buffer is null or expired");
        CC_ASSERT(b->usage().has(buffer_usage::accel_structure_build_input),
                  "create_buffer_table: a buffer needs buffer_usage::accel_structure_build_input");
        addresses.push_back({.address = _ctx.buffer_address_of(*b)});
    }
    auto buffer = create_buffer_from_data(cc::span<gpu_buffer_ref const>(addresses), buffer_usage::readonly_buffer);
    return std::make_shared<buffer_table const>(cc::vector<raw_buffer_handle>::create_copy_of(buffers), cc::move(buffer));
}

cluster_pool_handle context_persistent_scope::create_cluster_pool(cluster_pool_description const& desc)
{
    CC_ASSERT(_ctx.supports(feature::clustered_geometry), "create_cluster_pool: this context has no "
                                                          "sg::feature::clustered_geometry");
    CC_ASSERT(desc.max_clusters > 0 && desc.max_total_triangles > 0 && desc.max_total_vertices > 0,
              "create_cluster_pool: a pool holds at least one cluster, triangle and vertex");
    CC_ASSERT(desc.max_triangles_per_cluster >= 1 && desc.max_triangles_per_cluster <= max_cluster_triangles
                  && desc.max_vertices_per_cluster >= 1 && desc.max_vertices_per_cluster <= max_cluster_vertices,
              "create_cluster_pool: a cluster holds 1 to 256 triangles and vertices");
    CC_ASSERT(desc.max_geometry_index >= 0 && desc.max_geometry_index < (1 << 24),
              "create_cluster_pool: max_geometry_index must fit in 24 bits");
    CC_ASSERT(desc.flags.without(accel_build_flag::fast_trace | accel_build_flag::fast_build).is_empty()
                  && !desc.flags.has_all(accel_build_flag::fast_trace | accel_build_flag::fast_build),
              "create_cluster_pool: fast_trace or fast_build, and nothing else");
    auto r = _ctx.try_create_cluster_pool(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw allocation_exception("cluster pool allocation failed", 0, r.error());
}

// bind path
// binding_group_layout / pipeline_layout / compute_pipeline creation lives on ctx.uncached (see uncached.cc) — those are schemas / PSOs, not lifetime-scoped resources.
// binding_group is a real per-scope descriptor allocation.

binding_group_handle context_persistent_scope::create_binding_group(binding_group_layout_handle layout,
                                                                    cc::span<named_view const> views,
                                                                    cc::span<named_sampler const> samplers)
{
    auto r = try_create_binding_group(cc::move(layout), views, samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<binding_group_handle> context_persistent_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                    cc::span<named_view const> views,
                                                                                    cc::span<named_sampler const> samplers)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    if (auto unsupported
        = impl::find_unsupported_view(_ctx.supports(feature::float32_filtering), layout->bindings(), views);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    _ctx._stats.add(stat::binding_groups_created);
    auto const checked = _ctx.portability_checks();
    auto const traced = _ctx.hit_record_checks();
    auto group = _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::persistent);
    if (group.has_value() && checked)
        impl::record_buffer_uses(*group.value(), views);
    if (group.has_value() && traced)
        impl::record_tlases(*group.value(), views);
    return group;
}

binding_group_handle context_persistent_scope::create_binding_group(binding_group_layout_handle layout,
                                                                    cc::span<slotted_view const> views,
                                                                    cc::span<named_sampler const> samplers)
{
    auto r = try_create_binding_group(cc::move(layout), views, samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<binding_group_handle> context_persistent_scope::try_create_binding_group(binding_group_layout_handle layout,
                                                                                    cc::span<slotted_view const> views,
                                                                                    cc::span<named_sampler const> samplers)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    if (auto unsupported
        = impl::find_unsupported_view(_ctx.supports(feature::float32_filtering), layout->bindings(), views);
        unsupported.has_value())
        return cc::error(cc::move(unsupported.value()));
    _ctx._stats.add(stat::binding_groups_created);
    // Only a slot needs the layout to name its binding, and only where the checks are on.
    auto const checked = _ctx.portability_checks() ? layout : binding_group_layout_handle();
    auto const traced = _ctx.hit_record_checks();
    auto group = _ctx.try_create_binding_group(cc::move(layout), views, samplers, lifetime_scope::persistent);
    if (group.has_value() && checked != nullptr)
        impl::record_buffer_uses(*group.value(), *checked, views);
    if (group.has_value() && traced)
        impl::record_tlases(*group.value(), views);
    return group;
}

staging_binding_group_handle context_persistent_scope::create_staging_binding_group(binding_group_layout_handle layout,
                                                                                    int unbounded_capacity)
{
    auto r = try_create_staging_binding_group(cc::move(layout), unbounded_capacity);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw binding_group_exception(r.error());
}

cc::result<staging_binding_group_handle> context_persistent_scope::try_create_staging_binding_group(
    binding_group_layout_handle layout,
    int unbounded_capacity)
{
    if (unbounded_capacity < 0)
        return cc::error(cc::format("staging_binding_group: an unbounded capacity of {} elements", unbounded_capacity));
    auto unbounded = cc::string_view();
    if (layout != nullptr)
        for (auto const& b : layout->bindings())
            if (b.is_unbounded())
                unbounded = b.name;
    if (!unbounded.empty() && unbounded_capacity == 0)
        return cc::error(cc::format("staging_binding_group: '{}' is an unbounded array, and the group needs a capacity "
                                    "for it",
                                    unbounded));
    if (unbounded.empty() && unbounded_capacity != 0)
        return cc::error("staging_binding_group: a capacity for a layout without an unbounded array");
    auto created
        = _ctx.try_create_staging_binding_group(cc::move(layout), unbounded_capacity, lifetime_scope::persistent);
    if (created.has_value())
    {
        created.value()->_float32_filtering = _ctx.supports(feature::float32_filtering);
        created.value()->_records_buffer_uses = _ctx.portability_checks();
        created.value()->_records_tlases = _ctx.hit_record_checks();
    }
    return created;
}
} // namespace sg

sg::raw_view sg::context_persistent_scope::implicit_constants(cc::vector<byte> block)
{
    // A constants block is read in rows of 16 bytes, and a buffer holding one is sized in 256-byte units on dx12.
    auto const view_size = cc::align_up(block.size(), isize(16));
    auto const raw = create_raw_buffer(cc::align_up(view_size, constants_buffer_offset_alignment),
                                       buffer_usage::constants_buffer | buffer_usage::copy_dst);
    _ctx.upload.bytes_to_buffer(raw, cc::make_pinned_data(cc::move(block)));
    return raw_buffer_view{.bound_as = view_class::constants,
                           .shape = view_shape::constants_block,
                           .buffer = raw,
                           .offset_in_bytes = 0,
                           .size_in_bytes = view_size};
}
