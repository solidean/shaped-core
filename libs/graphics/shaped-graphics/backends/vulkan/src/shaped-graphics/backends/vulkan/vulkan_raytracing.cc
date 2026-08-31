// vulkan_raytracing: acceleration-structure builds (cmd.raytracing).
// Translates the backend-neutral geometry / instance inputs into VkAccelerationStructureGeometryKHR, sizes the result
// from vkGetAccelerationStructureBuildSizesKHR, and records the build.
// See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.
//
// The barrier policy is sg's rather than either API's, and carries over unchanged from dx12: only the *result* is an
// acceleration structure (accel_write), scratch is a plain storage buffer (shader_write), the geometry inputs are
// ordinary reads (shader_read), and a referenced BLAS is read as a structure (accel_read).
//
// Where Vulkan is simpler: a built structure is not pinned to a permanent state.
// D3D12 forbids transitioning an acceleration-structure resource at all, so dx12 has to keep those buffers out of the
// ordinary barrier path; Vulkan uses the same barriers as everything else.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_shader_table.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/barrier/access_inference.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] VkBuildAccelerationStructureFlagsKHR to_vk_build_flags(sg::accel_build_flags f)
{
    CC_ASSERT(!f.has_all(sg::accel_build_flag::fast_trace | sg::accel_build_flag::fast_build),
              "fast_trace and fast_build are mutually exclusive");
    VkBuildAccelerationStructureFlagsKHR out = 0;
    if (f.has(sg::accel_build_flag::fast_trace))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (f.has(sg::accel_build_flag::fast_build))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    if (f.has(sg::accel_build_flag::allow_update))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (f.has(sg::accel_build_flag::allow_compaction))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    if (f.has(sg::accel_build_flag::minimize_memory))
        out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    return out;
}

/// Downcast + validate a build-input buffer, asserting the common misuses.
[[nodiscard]] vulkan_buffer_handle require_build_input(sg::raw_buffer_handle const& buffer)
{
    CC_ASSERT(buffer != nullptr, "acceleration-structure build input buffer is null");
    auto const b = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(b != nullptr, "build input buffer is not a vulkan buffer");
    CC_ASSERT(!b->is_expired(), "build input buffer is a transient buffer used past its epoch (expired)");
    CC_ASSERT(b->usage().has(sg::buffer_usage::accel_structure_build_input),
              "acceleration-structure build input buffer must have buffer_usage::accel_structure_build_input");
    return b;
}
} // namespace

vulkan_blas::~vulkan_blas()
{
    if (_accel == VK_NULL_HANDLE)
        return;

    vulkan_expiring_resource expiring;
    auto* const ctx = &_ctx;
    expiring.finalizers.push_back(
        [ctx, accel = _accel]
        { ctx->_raytracing_functions.destroy_acceleration_structure(ctx->_device, accel, nullptr); });
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

vulkan_tlas::~vulkan_tlas()
{
    if (_accel == VK_NULL_HANDLE)
        return;

    vulkan_expiring_resource expiring;
    auto* const ctx = &_ctx;
    expiring.finalizers.push_back(
        [ctx, accel = _accel]
        { ctx->_raytracing_functions.destroy_acceleration_structure(ctx->_device, accel, nullptr); });
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

/// The three things every build needs once its geometry is described: the sizes, the result storage with its
/// structure object, and the scratch.
/// Shared by the BLAS and TLAS paths, which differ only in their geometry and their type.
vulkan_command_list::built_acceleration_structure vulkan_command_list::build_acceleration_structure(
    VkAccelerationStructureTypeKHR type,
    cc::span<VkAccelerationStructureGeometryKHR const> geometries,
    cc::span<u32 const> primitive_counts,
    cc::span<VkAccelerationStructureBuildRangeInfoKHR const> ranges,
    sg::accel_build_flags flags)
{
    auto const& rt = _ctx._raytracing_functions;

    auto geometry_info = VkAccelerationStructureBuildGeometryInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = type,
        .flags = to_vk_build_flags(flags),
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = u32(geometries.size()),
        .pGeometries = geometries.data(),
    };

    auto sizes = VkAccelerationStructureBuildSizesInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    rt.get_build_sizes(_ctx._device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &geometry_info,
                       primitive_counts.data(), &sizes);
    CC_ASSERT(sizes.accelerationStructureSize > 0, "the build-size query reported a zero-size result");

    // Persistent result (valid across epochs) + transient scratch (recycled once its epoch retires) — sg's policy,
    // and the same split dx12 makes.
    //
    // The scratch is over-allocated by the device's scratch alignment so its *offset* can be aligned up: sg's
    // allocator has no alignment argument, and a misaligned scratch address is a hard validation error rather than a
    // slow path.
    auto const scratch_alignment
        = isize(_ctx.acceleration_structure_properties().minAccelerationStructureScratchOffsetAlignment);
    auto const result_raw = _ctx.persistent.create_raw_buffer(isize(sizes.accelerationStructureSize),
                                                              sg::buffer_usage::accel_structure_storage);
    auto const scratch_raw = _ctx.transient.create_raw_buffer(isize(sizes.buildScratchSize) + scratch_alignment,
                                                              sg::buffer_usage::readwrite_buffer);
    auto const result = std::dynamic_pointer_cast<vulkan_buffer const>(result_raw);
    auto const scratch = std::dynamic_pointer_cast<vulkan_buffer const>(scratch_raw);
    CC_ASSERT(result != nullptr && scratch != nullptr, "acceleration-structure buffers are not vulkan buffers");

    // The structure object over the result buffer.
    // This is what dx12 has no counterpart for: there, the buffer's address *is* the structure.
    auto const create_info = VkAccelerationStructureCreateInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = result->_buffer,
        .offset = 0,
        .size = sizes.accelerationStructureSize,
        .type = type,
    };
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    VkResult const r = rt.create_acceleration_structure(_ctx._device, &create_info, nullptr, &accel);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateAccelerationStructureKHR failed");

    auto const address_info = VkAccelerationStructureDeviceAddressInfoKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = accel,
    };
    VkDeviceAddress const address = rt.get_device_address(_ctx._device, &address_info);

    // The scratch address, rounded up to the device's required alignment.
    VkDeviceAddress scratch_address = scratch->_device_address;
    if (scratch_alignment > 1)
    {
        auto const mask = VkDeviceAddress(scratch_alignment) - 1;
        scratch_address = (scratch_address + mask) & ~mask;
    }

    geometry_info.dstAccelerationStructure = accel;
    geometry_info.scratchData.deviceAddress = scratch_address;

    // Order the build.
    // Every access is on the accel_build stage.
    track_buffer_access(*result, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_write);
    track_buffer_access(*scratch, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_write);
    return {.info = geometry_info,
            .result = result,
            .accel = accel,
            .address = address,
            .size_in_bytes = isize(sizes.accelerationStructureSize),
            .build_scratch_size_in_bytes = isize(sizes.buildScratchSize),
            .update_scratch_size_in_bytes = isize(sizes.updateScratchSize),
            .ranges = cc::vector<VkAccelerationStructureBuildRangeInfoKHR>::create_copy_of(ranges)};
}

sg::blas_handle vulkan_command_list::raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                     sg::accel_build_flags flags)
{
    CC_ASSERT(_ctx.is_raytracing_supported(), "ray tracing is not supported on this device (check "
                                              "cmd.raytracing.is_supported())");
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");

    cc::vector<VkAccelerationStructureGeometryKHR> descs;
    cc::vector<u32> primitive_counts;
    cc::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    cc::vector<vulkan_buffer_handle> inputs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.vertex_count > 0, "triangle geometry needs a positive vertex_count");
        CC_ASSERT(g.vertex_stride_in_bytes > 0, "triangle geometry needs a positive vertex_stride_in_bytes");
        auto const verts = require_build_input(g.vertices);
        inputs.push_back(verts);

        auto triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = verts->_device_address + VkDeviceAddress(g.vertex_offset_in_bytes)},
            .vertexStride = VkDeviceSize(g.vertex_stride_in_bytes),
            // maxVertex is the highest index the build may read, so it is one less than the count.
            .maxVertex = u32(g.vertex_count - 1),
            .indexType = VK_INDEX_TYPE_NONE_KHR,
        };

        u32 primitives = 0;
        if (g.indices != nullptr)
        {
            CC_ASSERT(g.index_count > 0 && g.index_count % 3 == 0, "indexed triangles need index_count > 0 and a "
                                                                   "multiple of 3");
            auto const idx = require_build_input(g.indices);
            inputs.push_back(idx);
            triangles.indexType = g.index_type == sg::index_format::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = idx->_device_address + VkDeviceAddress(g.index_offset_in_bytes);
            primitives = u32(g.index_count / 3);
        }
        else
        {
            CC_ASSERT(g.vertex_count % 3 == 0, "non-indexed triangles need vertex_count to be a multiple of 3");
            primitives = u32(g.vertex_count / 3);
        }

        if (g.transform != nullptr)
        {
            auto const tf = require_build_input(g.transform);
            inputs.push_back(tf);
            triangles.transformData.deviceAddress = tf->_device_address + VkDeviceAddress(g.transform_offset_in_bytes);
        }

        descs.push_back(VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = {.triangles = triangles},
            .flags = g.is_opaque ? VkGeometryFlagsKHR(VK_GEOMETRY_OPAQUE_BIT_KHR) : VkGeometryFlagsKHR(0),
        });
        primitive_counts.push_back(primitives);
        ranges.push_back({.primitiveCount = primitives});
    }

    auto built = build_acceleration_structure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, descs, primitive_counts,
                                              ranges, flags);
    for (auto const& in : inputs)
        track_buffer_access(*in, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    record_acceleration_structure_build(built);

    return std::make_shared<vulkan_blas>(_ctx, built.result, built.accel, built.address, built.size_in_bytes,
                                         built.build_scratch_size_in_bytes, built.update_scratch_size_in_bytes, flags,
                                         int(geometries.size()));
}

sg::blas_handle vulkan_command_list::raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                                 sg::accel_build_flags flags)
{
    CC_ASSERT(_ctx.is_raytracing_supported(), "ray tracing is not supported on this device (check "
                                              "cmd.raytracing.is_supported())");
    CC_ASSERT(!geometries.empty(), "build_blas needs at least one geometry");

    cc::vector<VkAccelerationStructureGeometryKHR> descs;
    cc::vector<u32> primitive_counts;
    cc::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    cc::vector<vulkan_buffer_handle> inputs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.aabb_count > 0, "procedural geometry needs a positive aabb_count");
        CC_ASSERT(g.aabb_stride_in_bytes > 0 && g.aabb_stride_in_bytes % 8 == 0, "aabb_stride_in_bytes must be "
                                                                                 "positive and a multiple of 8");
        auto const aabbs = require_build_input(g.aabbs);
        inputs.push_back(aabbs);

        descs.push_back(VkAccelerationStructureGeometryKHR{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
            .geometry
            = {.aabbs = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                         .data = {.deviceAddress = aabbs->_device_address + VkDeviceAddress(g.aabb_offset_in_bytes)},
                         .stride = VkDeviceSize(g.aabb_stride_in_bytes)}},
            .flags = g.is_opaque ? VkGeometryFlagsKHR(VK_GEOMETRY_OPAQUE_BIT_KHR) : VkGeometryFlagsKHR(0),
        });
        primitive_counts.push_back(u32(g.aabb_count));
        ranges.push_back({.primitiveCount = u32(g.aabb_count)});
    }

    auto built = build_acceleration_structure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, descs, primitive_counts,
                                              ranges, flags);
    for (auto const& in : inputs)
        track_buffer_access(*in, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    record_acceleration_structure_build(built);

    return std::make_shared<vulkan_blas>(_ctx, built.result, built.accel, built.address, built.size_in_bytes,
                                         built.build_scratch_size_in_bytes, built.update_scratch_size_in_bytes, flags,
                                         int(geometries.size()));
}

sg::tlas_handle vulkan_command_list::raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                           sg::accel_build_flags flags)
{
    CC_ASSERT(_ctx.is_raytracing_supported(), "ray tracing is not supported on this device (check "
                                              "cmd.raytracing.is_supported())");
    CC_ASSERT(!instances.empty(), "build_tlas needs at least one instance");

    // VkAccelerationStructureInstanceKHR and D3D12_RAYTRACING_INSTANCE_DESC are the same 64-byte record by design, so
    // this packing is the dx12 one with the flag names changed.
    cc::vector<VkAccelerationStructureInstanceKHR> instance_descs;
    cc::vector<sg::blas_handle> referenced_blases;
    cc::vector<vulkan_buffer_handle> referenced_storage;

    for (auto const& inst : instances)
    {
        CC_ASSERT(inst.blas != nullptr, "tlas_instance.blas is null");
        CC_ASSERT(!inst.blas->is_expired(), "tlas_instance.blas is expired");
        CC_ASSERT(inst.instance_id < (1u << 24), "tlas_instance.instance_id must fit in 24 bits");
        CC_ASSERT(inst.hit_group_offset < (1u << 24), "tlas_instance.hit_group_offset must fit in 24 bits");
        auto const vk_blas = std::dynamic_pointer_cast<vulkan_blas const>(inst.blas);
        CC_ASSERT(vk_blas != nullptr, "tlas_instance.blas is not a vulkan blas");

        VkAccelerationStructureInstanceKHR d = {};
        for (int k = 0; k < 12; ++k) // both row-major 3x4: element k maps straight through
            (&d.transform.matrix[0][0])[k] = inst.transform[k];
        d.instanceCustomIndex = inst.instance_id;
        d.mask = inst.mask;
        d.instanceShaderBindingTableRecordOffset = inst.hit_group_offset;

        VkGeometryInstanceFlagsKHR iflags = 0;
        switch (inst.cull_mode)
        {
        case sg::instance_cull_mode::back:
            break; // default winding, no flag
        case sg::instance_cull_mode::front:
            iflags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
            break;
        case sg::instance_cull_mode::none:
            iflags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            break;
        }
        if (inst.opaque_override.has_value())
            iflags |= inst.opaque_override.value()
                        ? VkGeometryInstanceFlagsKHR(VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR)
                        : VkGeometryInstanceFlagsKHR(VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
        d.flags = iflags;
        d.accelerationStructureReference = u64(vk_blas->_address);

        instance_descs.push_back(d);
        referenced_blases.push_back(inst.blas);
        referenced_storage.push_back(vk_blas->_vulkan_storage);
    }

    // The packed instances go into a transient build-input buffer, written by the inline upload path.
    auto const instance_bytes = isize(instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    auto const instance_raw = _ctx.transient.create_raw_buffer(
        instance_bytes, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const instance_buf = std::dynamic_pointer_cast<vulkan_buffer const>(instance_raw);
    CC_ASSERT(instance_buf != nullptr, "instance buffer is not a vulkan buffer");

    // The ordinary inline-upload seam, which tracks and flushes the copy_write itself.
    upload_bytes_to_buffer(instance_raw,
                           cc::as_bytes(cc::span<VkAccelerationStructureInstanceKHR const>(instance_descs)), 0);

    auto const geometry = VkAccelerationStructureGeometryKHR{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = {.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .arrayOfPointers = VK_FALSE,
                                   .data = {.deviceAddress = instance_buf->_device_address}}},
    };
    u32 const primitive_count = u32(instances.size());
    auto const range = VkAccelerationStructureBuildRangeInfoKHR{.primitiveCount = primitive_count};

    auto built = build_acceleration_structure(
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, cc::span<VkAccelerationStructureGeometryKHR const>(&geometry, 1),
        cc::span<u32 const>(&primitive_count, 1), cc::span<VkAccelerationStructureBuildRangeInfoKHR const>(&range, 1),
        flags);

    // The instance descs are an ordinary buffer read; each referenced BLAS is read *as* an acceleration structure.
    track_buffer_access(*instance_buf, sg::pipeline_stage_flag::accel_build, sg::access_flag::shader_read);
    for (auto const& s : referenced_storage)
        track_buffer_access(*s, sg::pipeline_stage_flag::accel_build, sg::access_flag::accel_read);
    record_acceleration_structure_build(built);

    return std::make_shared<vulkan_tlas>(_ctx, built.result, built.accel, built.address, built.size_in_bytes,
                                         built.build_scratch_size_in_bytes, built.update_scratch_size_in_bytes, flags,
                                         int(instances.size()), cc::move(referenced_blases));
}

void vulkan_command_list::record_acceleration_structure_build(built_acceleration_structure const& built)
{
    flush_barriers();

    VkAccelerationStructureBuildRangeInfoKHR const* range_ptr = built.ranges.data();
    _ctx._raytracing_functions.cmd_build_acceleration_structures(_buffer, 1, &built.info, &range_ptr);
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
void vulkan_command_list::raytracing_bind_pipeline(sg::raytracing_pipeline const& pipeline)
{
    auto const* rp = dynamic_cast<vulkan_raytracing_pipeline const*>(&pipeline);
    CC_ASSERT(rp != nullptr, "raytracing_pipeline is not a vulkan raytracing_pipeline");

    auto const binding = VkDescriptorBufferBindingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = _ctx._descriptor_heap.device_address(),
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    _ctx._descriptor_functions.cmd_bind_descriptor_buffers(_buffer, 1, &binding);
    vkCmdBindPipeline(_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rp->_pipeline);

    _bound_pipeline_layout = rp->layout.get();
    _bound_groups.clear_resize_to_filled(_bound_pipeline_layout->_groups.size(), nullptr);
}

void vulkan_command_list::raytracing_bind_group(int group_index, sg::binding_group const& group)
{
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a raytracing pipeline before binding groups");
    CC_ASSERT(group_index >= 0 && group_index < int(_bound_groups.size()), "binding-group slot out of range for the "
                                                                           "bound pipeline layout");

    auto const* vg = dynamic_cast<vulkan_binding_group const*>(&group);
    CC_ASSERT(vg != nullptr, "binding_group is not a vulkan binding_group");
    CC_ASSERT(!(vg->transient && vg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");
    CC_ASSERT(vg->layout == _bound_pipeline_layout->_groups[group_index], "binding_group's layout does not match the "
                                                                          "pipeline layout's slot");
    auto const pinned = vg->layout->group_index();
    CC_ASSERTF(!pinned.has_value() || pinned.value() == u32(group_index),
               "binding_group is pinned to group index {} by its bindings and cannot be bound at slot {}",
               pinned.value_or(0), group_index);

    _bound_groups[group_index] = vg;

    u32 const buffer_index = 0;
    auto const offset = VkDeviceSize(vg->range.offset);
    _ctx._descriptor_functions.cmd_set_descriptor_offsets(_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                                          _bound_pipeline_layout->_layout, u32(group_index), 1,
                                                          &buffer_index, &offset);
}

void vulkan_command_list::raytracing_dispatch_rays(sg::raytracing_shader_table const& table,
                                                   sg::raygen_index raygen,
                                                   int width,
                                                   int height,
                                                   int depth)
{
    CC_ASSERT(width >= 1 && height >= 1 && depth >= 1, "dispatch_rays dimensions must be >= 1");
    CC_ASSERT(i64(width) * i64(height) * i64(depth) <= (i64(1) << 30), "dispatch_rays exceeds the 2^30 "
                                                                       "total-thread limit");
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a raytracing pipeline before dispatch_rays");
    CC_ASSERT(!_in_render_pass, "dispatch_rays must not be recorded inside a rendering scope; close the scope first");

    auto const* vt = dynamic_cast<vulkan_raytracing_shader_table const*>(&table);
    CC_ASSERT(vt != nullptr, "raytracing_shader_table is not a vulkan shader table");

    // Declare each bound group's accesses at the raytracing stage — a bound TLAS surfaces as accel_read — the same
    // rhythm as compute_dispatch.
    for (auto const* bound_group : _bound_groups)
    {
        if (bound_group == nullptr)
            continue;

        for (auto const& view : bound_group->hazard_views)
            if (view.buffer != nullptr)
                track_buffer_access(*view.buffer, sg::pipeline_stage_flag::raytracing, sg::shader_access_of(view.access));
        for (auto const& tv : bound_group->texture_hazard_views)
            (void)track_texture_access(*tv.texture, tv.range, sg::pipeline_stage_flag::raytracing,
                                       sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    declare_array_accesses();

    // The shader table buffer is read by the fixed-function ray dispatch.
    track_buffer_access(*vt->buffer, sg::pipeline_stage_flag::raytracing, sg::access_flag::shader_read);
    flush_barriers();

    auto const raygen_region = vt->raygen_record(raygen);
    _ctx._raytracing_functions.cmd_trace_rays(_buffer, &raygen_region, &vt->miss_table, &vt->hit_table,
                                              &vt->callable_table, u32(width), u32(height), u32(depth));
}
} // namespace sg::backend::vulkan
