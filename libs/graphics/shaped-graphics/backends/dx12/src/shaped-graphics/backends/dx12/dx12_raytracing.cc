// dx12_raytracing: ray-tracing acceleration-structure builds (cmd.raytracing). Translates the backend-neutral
// geometry / instance inputs to DXR descs, sizes the result from a prebuild query, and records the build via
// ID3D12GraphicsCommandList4. See libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md.

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_dxr_build_flags(sg::accel_build_flags f)
{
    CC_ASSERT(!(sg::has_flag(f, sg::accel_build_flags::fast_trace) && sg::has_flag(f, sg::accel_build_flags::fast_build)),
              "fast_trace and fast_build are mutually exclusive");
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS out = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    if (sg::has_flag(f, sg::accel_build_flags::fast_trace))
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (sg::has_flag(f, sg::accel_build_flags::fast_build))
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    if (sg::has_flag(f, sg::accel_build_flags::allow_update))
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (sg::has_flag(f, sg::accel_build_flags::allow_compaction))
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    if (sg::has_flag(f, sg::accel_build_flags::minimize_memory))
        out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    return out;
}

// Downcast + validate a build-input buffer, returning its dx12_buffer (asserts on the common misuses).
[[nodiscard]] dx12_buffer_handle require_build_input(sg::raw_buffer_handle const& buffer, char const* what)
{
    CC_ASSERT(buffer != nullptr, "acceleration-structure build input buffer is null");
    auto const b = std::dynamic_pointer_cast<dx12_buffer const>(buffer);
    CC_ASSERT(b != nullptr, "build input buffer is not a dx12 buffer");
    CC_ASSERT(!b->is_expired(), "build input buffer is a transient buffer used past its epoch (expired)");
    CC_ASSERT(sg::has_flag(b->usage(), sg::buffer_usage::accel_structure_build_input),
              "acceleration-structure build input buffer must have buffer_usage::accel_structure_build_input");
    (void)what;
    return b;
}
} // namespace

bool dx12_command_list::raytracing_is_supported() const
{
    return _ctx.supports_raytracing();
}

sg::blas_handle dx12_command_list::build_blas_common(cc::span<D3D12_RAYTRACING_GEOMETRY_DESC const> geometry_descs,
                                                     cc::span<dx12_buffer_handle const> input_buffers,
                                                     sg::accel_build_flags flags,
                                                     int geometry_count)
{
    CC_ASSERT(_ctx.supports_raytracing(), "ray tracing is not supported on this device (check "
                                          "cmd.raytracing.is_supported())");
    CC_ASSERT(!geometry_descs.empty(), "build_blas needs at least one geometry");

    // Query the DXR interfaces off the base device / list. Do the QueryInterface OUTSIDE the assert — its
    // out-param is a real side effect that would be compiled out with CC_ASSERT in asserts-off builds.
    ComPtr<ID3D12Device5> device5;
    [[maybe_unused]] HRESULT const device5_hr = _ctx._device.As(&device5);
    CC_ASSERT(SUCCEEDED(device5_hr) && device5, "ID3D12Device5 unavailable (SDK/driver too old for DXR)");
    ComPtr<ID3D12GraphicsCommandList4> list4;
    [[maybe_unused]] HRESULT const list4_hr = _list.As(&list4);
    CC_ASSERT(SUCCEEDED(list4_hr) && list4, "ID3D12GraphicsCommandList4 unavailable (SDK/driver too old for DXR)");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = to_dxr_build_flags(flags);
    inputs.NumDescs = UINT(geometry_descs.size());
    inputs.pGeometryDescs = geometry_descs.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    CC_ASSERT(prebuild.ResultDataMaxSizeInBytes > 0, "prebuild reported a zero-size BLAS result");

    // Persistent result (across-epoch) + transient scratch (recycled once its epoch retires).
    auto const result_raw = _ctx.persistent.create_raw_buffer(cc::isize(prebuild.ResultDataMaxSizeInBytes),
                                                              sg::buffer_usage::accel_structure_storage);
    auto const scratch_raw = _ctx.transient.create_raw_buffer(cc::isize(prebuild.ScratchDataSizeInBytes),
                                                              sg::buffer_usage::readwrite_buffer);
    auto const result = std::dynamic_pointer_cast<dx12_buffer const>(result_raw);
    auto const scratch = std::dynamic_pointer_cast<dx12_buffer const>(scratch_raw);
    CC_ASSERT(result != nullptr && scratch != nullptr, "acceleration-structure buffers are not dx12 buffers");

    // Order the build, all on the accel_build stage. Only the AS *result* is an acceleration structure
    // (accel_write); scratch is a plain UAV (shader_write / UNORDERED_ACCESS) and the geometry inputs are
    // ordinary buffer reads (shader_read / SHADER_RESOURCE) — the AS access bits are illegal on non-AS
    // buffers. The tracker turns a prior upload of the inputs (copy_write) into the copy->read barrier.
    track_buffer_access(result, sg::pipeline_stage_flags::accel_build, sg::access_flags::accel_write);
    track_buffer_access(scratch, sg::pipeline_stage_flags::accel_build, sg::access_flags::shader_write);
    for (auto const& in : input_buffers)
        track_buffer_access(in, sg::pipeline_stage_flags::accel_build, sg::access_flags::shader_read);
    flush_barriers();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
    build_desc.DestAccelerationStructureData = result->gpu_virtual_address();
    build_desc.ScratchAccelerationStructureData = scratch->gpu_virtual_address();
    build_desc.Inputs = inputs;
    list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

    return std::make_shared<dx12_blas>(result, cc::isize(prebuild.ResultDataMaxSizeInBytes),
                                       cc::isize(prebuild.ScratchDataSizeInBytes),
                                       cc::isize(prebuild.UpdateScratchDataSizeInBytes), flags, geometry_count);
}

sg::blas_handle dx12_command_list::raytracing_build_blas_triangles(cc::span<sg::blas_triangles const> geometries,
                                                                   sg::accel_build_flags flags)
{
    cc::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    descs.reserve(geometries.size());
    cc::vector<dx12_buffer_handle> inputs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.vertex_count > 0, "triangle geometry needs a positive vertex_count");
        CC_ASSERT(g.vertex_stride_in_bytes > 0, "triangle geometry needs a positive vertex_stride_in_bytes");
        auto const verts = require_build_input(g.vertices, "vertices");
        inputs.push_back(verts);

        D3D12_RAYTRACING_GEOMETRY_DESC d = {};
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = g.is_opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        d.Triangles.VertexBuffer.StartAddress = verts->gpu_virtual_address() + UINT64(g.vertex_offset_in_bytes);
        d.Triangles.VertexBuffer.StrideInBytes = UINT64(g.vertex_stride_in_bytes);
        d.Triangles.VertexCount = UINT(g.vertex_count);
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

        if (g.indices != nullptr)
        {
            CC_ASSERT(g.index_count > 0 && g.index_count % 3 == 0, "indexed triangles need index_count > 0 and a "
                                                                   "multiple of 3");
            auto const idx = require_build_input(g.indices, "indices");
            inputs.push_back(idx);
            d.Triangles.IndexBuffer = idx->gpu_virtual_address() + UINT64(g.index_offset_in_bytes);
            d.Triangles.IndexCount = UINT(g.index_count);
            d.Triangles.IndexFormat
                = g.index_format == sg::accel_index_format::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        }
        else
        {
            CC_ASSERT(g.vertex_count % 3 == 0, "non-indexed triangles need vertex_count to be a multiple of 3");
        }

        if (g.transform != nullptr)
        {
            auto const tf = require_build_input(g.transform, "transform");
            inputs.push_back(tf);
            d.Triangles.Transform3x4 = tf->gpu_virtual_address() + UINT64(g.transform_offset_in_bytes);
        }

        descs.push_back(d);
    }

    return build_blas_common(descs, inputs, flags, int(geometries.size()));
}

sg::blas_handle dx12_command_list::raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const> geometries,
                                                               sg::accel_build_flags flags)
{
    cc::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    descs.reserve(geometries.size());
    cc::vector<dx12_buffer_handle> inputs;

    for (auto const& g : geometries)
    {
        CC_ASSERT(g.aabb_count > 0, "procedural geometry needs a positive aabb_count");
        CC_ASSERT(g.aabb_stride_in_bytes > 0 && g.aabb_stride_in_bytes % 8 == 0, "aabb_stride_in_bytes must be "
                                                                                 "positive and a multiple of 8");
        auto const aabbs = require_build_input(g.aabbs, "aabbs");
        inputs.push_back(aabbs);

        D3D12_RAYTRACING_GEOMETRY_DESC d = {};
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        d.Flags = g.is_opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        d.AABBs.AABBCount = UINT64(g.aabb_count);
        d.AABBs.AABBs.StartAddress = aabbs->gpu_virtual_address() + UINT64(g.aabb_offset_in_bytes);
        d.AABBs.AABBs.StrideInBytes = UINT64(g.aabb_stride_in_bytes);

        descs.push_back(d);
    }

    return build_blas_common(descs, inputs, flags, int(geometries.size()));
}

sg::tlas_handle dx12_command_list::raytracing_build_tlas(cc::span<sg::tlas_instance const> instances,
                                                         sg::accel_build_flags flags)
{
    CC_ASSERT(_ctx.supports_raytracing(), "ray tracing is not supported on this device (check "
                                          "cmd.raytracing.is_supported())");
    CC_ASSERT(!instances.empty(), "build_tlas needs at least one instance");

    // QueryInterface OUTSIDE the assert — the out-param is a side effect (see build_blas_common).
    ComPtr<ID3D12Device5> device5;
    [[maybe_unused]] HRESULT const device5_hr = _ctx._device.As(&device5);
    CC_ASSERT(SUCCEEDED(device5_hr) && device5, "ID3D12Device5 unavailable (SDK/driver too old for DXR)");
    ComPtr<ID3D12GraphicsCommandList4> list4;
    [[maybe_unused]] HRESULT const list4_hr = _list.As(&list4);
    CC_ASSERT(SUCCEEDED(list4_hr) && list4, "ID3D12GraphicsCommandList4 unavailable (SDK/driver too old for DXR)");

    // Pack the instances into DXR descs, collecting the referenced BLASes (to keep alive + barrier accel_read).
    cc::vector<D3D12_RAYTRACING_INSTANCE_DESC> instance_descs;
    instance_descs.reserve(instances.size());
    cc::vector<sg::blas_handle> referenced_blases;
    cc::vector<dx12_buffer_handle> referenced_storage;

    for (auto const& inst : instances)
    {
        CC_ASSERT(inst.blas != nullptr, "tlas_instance.blas is null");
        CC_ASSERT(!inst.blas->is_expired(), "tlas_instance.blas is expired");
        CC_ASSERT(inst.instance_id < (1u << 24), "tlas_instance.instance_id must fit in 24 bits");
        CC_ASSERT(inst.hit_group_offset < (1u << 24), "tlas_instance.hit_group_offset must fit in 24 bits");
        auto const dx_blas = std::dynamic_pointer_cast<dx12_blas const>(inst.blas);
        CC_ASSERT(dx_blas != nullptr, "tlas_instance.blas is not a dx12 blas");

        D3D12_RAYTRACING_INSTANCE_DESC d = {};
        for (int k = 0; k < 12; ++k) // both row-major 3x4: element k maps straight through
            (&d.Transform[0][0])[k] = inst.transform[k];
        d.InstanceID = inst.instance_id;
        d.InstanceMask = inst.mask;
        d.InstanceContributionToHitGroupIndex = inst.hit_group_offset;

        D3D12_RAYTRACING_INSTANCE_FLAGS iflags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        switch (inst.cull_mode)
        {
        case sg::instance_cull_mode::back:
            break; // default winding, no flag
        case sg::instance_cull_mode::front:
            iflags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
            break;
        case sg::instance_cull_mode::none:
            iflags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            break;
        }
        if (inst.opaque_override.has_value())
            iflags |= inst.opaque_override.value() ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE
                                                   : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        d.Flags = UINT(iflags);
        d.AccelerationStructure = dx_blas->_dx12_storage->gpu_virtual_address();

        instance_descs.push_back(d);
        referenced_blases.push_back(inst.blas);
        referenced_storage.push_back(dx_blas->_dx12_storage);
    }

    // Upload the packed descs into a transient build-input buffer (copy_dst so the inline upload can write it).
    auto const instance_bytes = cc::isize(instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    auto const instance_raw = _ctx.transient.create_raw_buffer(
        instance_bytes, sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    auto const instance_buf = std::dynamic_pointer_cast<dx12_buffer const>(instance_raw);
    CC_ASSERT(instance_buf != nullptr, "instance buffer is not a dx12 buffer");

    track_buffer_access(instance_buf, sg::pipeline_stage_flags::copy, sg::access_flags::copy_write);
    flush_barriers();
    _ctx._upload_inline.upload_buffer(*this, *instance_buf,
                                      cc::as_bytes(cc::span<D3D12_RAYTRACING_INSTANCE_DESC const>(instance_descs)), 0);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = to_dxr_build_flags(flags);
    inputs.NumDescs = UINT(instances.size());
    inputs.InstanceDescs = instance_buf->gpu_virtual_address();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    CC_ASSERT(prebuild.ResultDataMaxSizeInBytes > 0, "prebuild reported a zero-size TLAS result");

    auto const result_raw = _ctx.persistent.create_raw_buffer(cc::isize(prebuild.ResultDataMaxSizeInBytes),
                                                              sg::buffer_usage::accel_structure_storage);
    auto const scratch_raw = _ctx.transient.create_raw_buffer(cc::isize(prebuild.ScratchDataSizeInBytes),
                                                              sg::buffer_usage::readwrite_buffer);
    auto const result = std::dynamic_pointer_cast<dx12_buffer const>(result_raw);
    auto const scratch = std::dynamic_pointer_cast<dx12_buffer const>(scratch_raw);
    CC_ASSERT(result != nullptr && scratch != nullptr, "acceleration-structure buffers are not dx12 buffers");

    // The top-level build writes the result (accel_write), reads the instance descs as an ordinary buffer
    // (shader_read — it is not an acceleration structure), uses scratch as a UAV (shader_write), and reads
    // each referenced BLAS *as an acceleration structure* (accel_read).
    track_buffer_access(result, sg::pipeline_stage_flags::accel_build, sg::access_flags::accel_write);
    track_buffer_access(scratch, sg::pipeline_stage_flags::accel_build, sg::access_flags::shader_write);
    track_buffer_access(instance_buf, sg::pipeline_stage_flags::accel_build, sg::access_flags::shader_read);
    for (auto const& s : referenced_storage)
        track_buffer_access(s, sg::pipeline_stage_flags::accel_build, sg::access_flags::accel_read);
    flush_barriers();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc = {};
    build_desc.DestAccelerationStructureData = result->gpu_virtual_address();
    build_desc.ScratchAccelerationStructureData = scratch->gpu_virtual_address();
    build_desc.Inputs = inputs;
    list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

    return std::make_shared<dx12_tlas>(
        result, cc::isize(prebuild.ResultDataMaxSizeInBytes), cc::isize(prebuild.ScratchDataSizeInBytes),
        cc::isize(prebuild.UpdateScratchDataSizeInBytes), flags, int(instances.size()), cc::move(referenced_blases));
}
} // namespace sg::backend::dx12
