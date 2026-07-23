#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_barrier.hh>

// Buffer barrier emission — specifically the sync/access reconciliation D3D12 validates pairwise.
// make_buffer_barrier only fills a struct, so none of this needs a device.

namespace
{
namespace dx12 = sg::backend::dx12;

/// A barrier from a copy write to `dst_stages`/`dst_access`, the shape a draw produces after an upload.
[[nodiscard]] D3D12_BUFFER_BARRIER after_copy_to(sg::pipeline_stage_flags dst_stages, sg::access_flags dst_access)
{
    return dx12::make_buffer_barrier(nullptr, {.needed = true,
                                               .src_stages = sg::pipeline_stage_flags::copy,
                                               .src_access = sg::access_flags::copy_write,
                                               .dst_stages = dst_stages,
                                               .dst_access = dst_access});
}
} // namespace

TEST("sg dx12 - an index-buffer read syncs on INDEX_INPUT, never VERTEX_SHADING")
{
    // D3D12 validates sync against access pairwise: ACCESS_INDEX_BUFFER paired with SYNC_VERTEX_SHADING is rejected outright ("incompatible"),
    // because the index fetch happens in the input assembler.
    // sg's stage vocabulary has no input-assembler stage, so `vertex` is what the draw declares — and the translation has to correct it.
    // Merely adding INDEX_INPUT is not enough; the VERTEX_SHADING bit must be gone.
    auto const b = after_copy_to(sg::pipeline_stage_flags::vertex, sg::access_flags::index_read);

    CHECK((b.AccessAfter & D3D12_BARRIER_ACCESS_INDEX_BUFFER) != 0);
    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_INDEX_INPUT) != 0);
    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_VERTEX_SHADING) == 0);
}

TEST("sg dx12 - a vertex-buffer read still syncs on VERTEX_SHADING")
{
    // The neighbouring case must not regress: ACCESS_VERTEX_BUFFER pairs with SYNC_VERTEX_SHADING fine.
    auto const b = after_copy_to(sg::pipeline_stage_flags::vertex, sg::access_flags::vertex_read);

    CHECK((b.AccessAfter & D3D12_BARRIER_ACCESS_VERTEX_BUFFER) != 0);
    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_VERTEX_SHADING) != 0);
    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_INDEX_INPUT) == 0);
}

TEST("sg dx12 - one buffer read as both index and vertex widens to SYNC_DRAW")
{
    // The transient allocator can hand a draw's vertex and index data out of one resource, so both accesses merge into a single barrier.
    // No narrow sync bit is legal with both, and SYNC_DRAW covers the pair.
    auto const b
        = after_copy_to(sg::pipeline_stage_flags::vertex, sg::access_flags::index_read | sg::access_flags::vertex_read);

    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_DRAW) != 0);
    CHECK((b.SyncAfter & D3D12_BARRIER_SYNC_VERTEX_SHADING) == 0);
}

TEST("sg dx12 - a copy-only barrier is untouched")
{
    // The reconciliation must not fire when no index read is involved.
    auto const b = after_copy_to(sg::pipeline_stage_flags::copy, sg::access_flags::copy_read);

    CHECK(b.SyncAfter == D3D12_BARRIER_SYNC_COPY);
    CHECK(b.AccessAfter == D3D12_BARRIER_ACCESS_COPY_SOURCE);
}
