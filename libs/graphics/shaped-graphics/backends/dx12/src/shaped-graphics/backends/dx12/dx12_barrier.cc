#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>

namespace sg::backend::dx12
{
D3D12_BARRIER_SYNC d3d12_sync_from(sg::pipeline_stage_flags stages)
{
    D3D12_BARRIER_SYNC out = D3D12_BARRIER_SYNC_NONE;
    if (stages.has(sg::pipeline_stage_flag::draw_indirect))
        out |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    if (stages.has(sg::pipeline_stage_flag::vertex))
        out |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    if (stages.has(sg::pipeline_stage_flag::fragment))
        out |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    if (stages.has(sg::pipeline_stage_flag::compute))
        out |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    if (stages.has(sg::pipeline_stage_flag::copy))
        out |= D3D12_BARRIER_SYNC_COPY;
    if (stages.has(sg::pipeline_stage_flag::render_target))
        out |= D3D12_BARRIER_SYNC_RENDER_TARGET;
    if (stages.has(sg::pipeline_stage_flag::depth_stencil_target))
        out |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    if (stages.has(sg::pipeline_stage_flag::raytracing))
        out |= D3D12_BARRIER_SYNC_RAYTRACING;
    if (stages.has(sg::pipeline_stage_flag::accel_build))
        out |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
    return out;
}

D3D12_BARRIER_ACCESS d3d12_access_from(sg::access_flags access)
{
    D3D12_BARRIER_ACCESS out = D3D12_BARRIER_ACCESS_COMMON; // 0
    if (access.has(sg::access_flag::uniform_read))
        out |= D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    if (access.has(sg::access_flag::index_read))
        out |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
    if (access.has(sg::access_flag::vertex_read))
        out |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
    if (access.has(sg::access_flag::shader_read))
        out |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    if (access.has(sg::access_flag::shader_write))
        out |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    if (access.has(sg::access_flag::copy_read))
        out |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
    if (access.has(sg::access_flag::copy_write))
        out |= D3D12_BARRIER_ACCESS_COPY_DEST;
    if (access.has(sg::access_flag::indirect_read))
        out |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    if (access.has(sg::access_flag::color_write))
        out |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
    if (access.has(sg::access_flag::depth_read))
        out |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    if (access.has(sg::access_flag::depth_write))
        out |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    if (access.has(sg::access_flag::accel_read))
        out |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
    if (access.has(sg::access_flag::accel_write))
        out |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
    return out;
}

namespace
{
/// Reconcile a stage-derived sync with the access it is paired with, for index-buffer reads.
///
/// D3D12 validates sync against access pairwise, and ACCESS_INDEX_BUFFER pairs only with SYNC_INDEX_INPUT or the broader SYNC_DRAW —
/// the index fetch happens in the input assembler, not in vertex shading.
/// Pairing it with SYNC_VERTEX_SHADING is rejected outright, and *adding* INDEX_INPUT does not help:
/// the offending VERTEX_SHADING x INDEX_BUFFER pair is still there.
/// So VERTEX_SHADING has to go.
///
/// sg's stage vocabulary has no input-assembler stage (`vertex` covers all of vertex processing), which is why this cannot be decided in d3d12_sync_from —
/// only here is the access known.
/// When the same barrier also carries an access that genuinely needs vertex shading (one buffer bound as both vertex and index, as the transient allocator can produce),
/// SYNC_DRAW is the one bit legal with both.
[[nodiscard]] D3D12_BARRIER_SYNC sync_for_access(D3D12_BARRIER_SYNC sync, sg::access_flags access)
{
    if (sync == D3D12_BARRIER_SYNC_NONE || !access.has(sg::access_flag::index_read))
        return sync;
    if ((sync & D3D12_BARRIER_SYNC_VERTEX_SHADING) == 0)
        return sync | D3D12_BARRIER_SYNC_INDEX_INPUT;

    auto const vertex_stage_access
        = sg::access_flag::vertex_read | sg::access_flag::uniform_read | sg::access_flag::shader_read;
    auto const also_shades = access.has_any(vertex_stage_access);

    sync &= ~D3D12_BARRIER_SYNC_VERTEX_SHADING;
    return sync | (also_shades ? D3D12_BARRIER_SYNC_DRAW : D3D12_BARRIER_SYNC_INDEX_INPUT);
}
} // namespace

D3D12_BUFFER_BARRIER make_buffer_barrier(ID3D12Resource* resource, sg::access_barrier const& b)
{
    CC_ASSERT(b.needed, "make_buffer_barrier called with no barrier to build");

    D3D12_BUFFER_BARRIER bb = {};
    bb.SyncBefore = sync_for_access(d3d12_sync_from(b.src_stages), b.src_access);
    bb.SyncAfter = sync_for_access(d3d12_sync_from(b.dst_stages), b.dst_access);
    // D3D12 rule: SYNC_NONE pairs with ACCESS_NO_ACCESS.
    // With a real stage set we translate the access.
    bb.AccessBefore
        = bb.SyncBefore == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.src_access);
    bb.AccessAfter
        = bb.SyncAfter == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.dst_access);
    bb.pResource = resource;
    bb.Offset = 0;
    bb.Size = UINT64_MAX; // whole buffer — D3D12 buffer barriers cannot cover a sub-range
    return bb;
}

D3D12_BARRIER_LAYOUT d3d12_layout_from(sg::texture_layout layout)
{
    switch (layout)
    {
    case sg::texture_layout::undefined:
        return D3D12_BARRIER_LAYOUT_UNDEFINED;
    case sg::texture_layout::general:
        return D3D12_BARRIER_LAYOUT_COMMON;
    case sg::texture_layout::shader_readonly:
        return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    case sg::texture_layout::shader_readwrite:
        return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    case sg::texture_layout::render_target:
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    case sg::texture_layout::depth_readonly:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    case sg::texture_layout::depth_readwrite:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    case sg::texture_layout::copy_src:
        return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    case sg::texture_layout::copy_dst:
        return D3D12_BARRIER_LAYOUT_COPY_DEST;
    case sg::texture_layout::present:
        return D3D12_BARRIER_LAYOUT_PRESENT;
    }
    CC_ASSERT(false, "unhandled texture_layout in d3d12_layout_from");
    return D3D12_BARRIER_LAYOUT_COMMON;
}

D3D12_TEXTURE_BARRIER make_texture_barrier(ID3D12Resource* resource,
                                           sg::subresource_range const& range,
                                           sg::access_barrier const& b)
{
    CC_ASSERT(b.needed, "make_texture_barrier called with no barrier to build");

    D3D12_TEXTURE_BARRIER tb = {};
    tb.SyncBefore = d3d12_sync_from(b.src_stages);
    tb.SyncAfter = d3d12_sync_from(b.dst_stages);
    // D3D12 rule: SYNC_NONE pairs with ACCESS_NO_ACCESS (a layout-only transition at the end of a list has no subsequent access).
    // With a real stage set we translate the access.
    tb.AccessBefore
        = tb.SyncBefore == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.src_access);
    tb.AccessAfter
        = tb.SyncAfter == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_ACCESS_NO_ACCESS : d3d12_access_from(b.dst_access);
    tb.LayoutBefore = d3d12_layout_from(b.src_layout);
    tb.LayoutAfter = d3d12_layout_from(b.dst_layout);
    tb.pResource = resource;
    tb.Subresources.IndexOrFirstMipLevel = UINT(range.mip_range.start);
    tb.Subresources.NumMipLevels = UINT(range.mip_range.end - range.mip_range.start);
    tb.Subresources.FirstArraySlice = UINT(range.array_range.start);
    tb.Subresources.NumArraySlices = UINT(range.array_range.end - range.array_range.start);
    tb.Subresources.FirstPlane = UINT(range.aspect_range.start);
    tb.Subresources.NumPlanes = UINT(range.aspect_range.end - range.aspect_range.start);
    // An `undefined` source layout means prior contents are not preserved — discard for a cheaper transition.
    tb.Flags = b.src_layout == sg::texture_layout::undefined ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                                                             : D3D12_TEXTURE_BARRIER_FLAG_NONE;
    return tb;
}

void submit_barriers(ID3D12GraphicsCommandList* list,
                     cc::span<D3D12_BUFFER_BARRIER const> buffer_barriers,
                     cc::span<D3D12_TEXTURE_BARRIER const> texture_barriers)
{
    if (buffer_barriers.empty() && texture_barriers.empty())
        return;

    ComPtr<ID3D12GraphicsCommandList7> list7;
    HRESULT const hr = list->QueryInterface(IID_PPV_ARGS(&list7));
    CC_ASSERT(SUCCEEDED(hr) && list7, "enhanced barriers require ID3D12GraphicsCommandList7 (SDK/driver too old)");

    // One barrier group per type;
    // both go into a single Barrier call so the whole operation's hazards are resolved at once.
    D3D12_BARRIER_GROUP groups[2] = {};
    UINT num_groups = 0;
    if (!buffer_barriers.empty())
    {
        auto& g = groups[num_groups++];
        g.Type = D3D12_BARRIER_TYPE_BUFFER;
        g.NumBarriers = UINT(buffer_barriers.size());
        g.pBufferBarriers = buffer_barriers.data();
    }
    if (!texture_barriers.empty())
    {
        auto& g = groups[num_groups++];
        g.Type = D3D12_BARRIER_TYPE_TEXTURE;
        g.NumBarriers = UINT(texture_barriers.size());
        g.pTextureBarriers = texture_barriers.data();
    }
    list7->Barrier(num_groups, groups);
}
} // namespace sg::backend::dx12
