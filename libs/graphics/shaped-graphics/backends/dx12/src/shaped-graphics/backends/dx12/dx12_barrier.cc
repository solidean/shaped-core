#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>

namespace sg::backend::dx12
{
D3D12_BARRIER_SYNC d3d12_sync_from(sg::pipeline_stage_flags stages)
{
    D3D12_BARRIER_SYNC out = D3D12_BARRIER_SYNC_NONE;
    if (sg::has_all(stages, sg::pipeline_stage_flags::draw_indirect))
        out |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    if (sg::has_all(stages, sg::pipeline_stage_flags::vertex))
        out |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    if (sg::has_all(stages, sg::pipeline_stage_flags::fragment))
        out |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    if (sg::has_all(stages, sg::pipeline_stage_flags::compute))
        out |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    if (sg::has_all(stages, sg::pipeline_stage_flags::transfer))
        out |= D3D12_BARRIER_SYNC_COPY;
    if (sg::has_all(stages, sg::pipeline_stage_flags::attachment))
        out |= D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    if (sg::has_all(stages, sg::pipeline_stage_flags::raytracing))
        out |= D3D12_BARRIER_SYNC_RAYTRACING;
    if (sg::has_all(stages, sg::pipeline_stage_flags::accel_build))
        out |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
    return out;
}

D3D12_BARRIER_ACCESS d3d12_access_from(sg::access_flags access)
{
    D3D12_BARRIER_ACCESS out = D3D12_BARRIER_ACCESS_COMMON; // 0
    if (sg::has_all(access, sg::access_flags::uniform_read))
        out |= D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    if (sg::has_all(access, sg::access_flags::index_read))
        out |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
    if (sg::has_all(access, sg::access_flags::vertex_read))
        out |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
    if (sg::has_all(access, sg::access_flags::shader_read))
        out |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    if (sg::has_all(access, sg::access_flags::shader_write))
        out |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    if (sg::has_all(access, sg::access_flags::transfer_read))
        out |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
    if (sg::has_all(access, sg::access_flags::transfer_write))
        out |= D3D12_BARRIER_ACCESS_COPY_DEST;
    if (sg::has_all(access, sg::access_flags::indirect_read))
        out |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    if (sg::has_all(access, sg::access_flags::color_write))
        out |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
    if (sg::has_all(access, sg::access_flags::depth_read))
        out |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    if (sg::has_all(access, sg::access_flags::depth_write))
        out |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    if (sg::has_all(access, sg::access_flags::accel_read))
        out |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
    if (sg::has_all(access, sg::access_flags::accel_write))
        out |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
    return out;
}

void emit_buffer_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, sg::access_barrier const& b)
{
    CC_ASSERT(b.needed, "emit_buffer_barrier called with no barrier to emit");

    ComPtr<ID3D12GraphicsCommandList7> list7;
    HRESULT const hr = list->QueryInterface(IID_PPV_ARGS(&list7));
    CC_ASSERT(SUCCEEDED(hr) && list7, "enhanced barriers require ID3D12GraphicsCommandList7 (SDK/driver too old)");

    D3D12_BUFFER_BARRIER bb = {};
    bb.SyncBefore = d3d12_sync_from(b.src_stages);
    bb.SyncAfter = d3d12_sync_from(b.dst_stages);
    // D3D12 rule: SYNC_NONE pairs with ACCESS_NO_ACCESS. With a real stage set we translate the access.
    bb.AccessBefore
        = bb.SyncBefore == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.src_access);
    bb.AccessAfter
        = bb.SyncAfter == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.dst_access);
    bb.pResource = resource;
    bb.Offset = 0;
    bb.Size = UINT64_MAX; // whole buffer — D3D12 buffer barriers cannot cover a sub-range

    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1;
    group.pBufferBarriers = &bb;
    list7->Barrier(1, &group);
}
} // namespace sg::backend::dx12
