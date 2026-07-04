// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here. Allocators are epoch-gated (recycled once the
// epoch retires); see libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
void dx12_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    auto const* dp = dynamic_cast<dx12_compute_pipeline const*>(&pipeline);
    CC_ASSERT(dp != nullptr, "compute_pipeline is not a dx12 compute_pipeline");

    // The shader-visible heap must be set before any root descriptor table is bound.
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get()};
    _list->SetDescriptorHeaps(1, heaps);
    _list->SetComputeRootSignature(dp->layout->root_signature.Get());
    _list->SetPipelineState(dp->pipeline_state.Get());
}

void dx12_command_list::compute_bind_group(int set, sg::binding_group const& group)
{
    CC_ASSERT(set == 0, "only descriptor set 0 is supported yet");
    auto const* dg = dynamic_cast<dx12_binding_group const*>(&group);
    CC_ASSERT(dg != nullptr, "binding_group is not a dx12 binding_group");

    // Buffers are in COMMON and implicitly promote to UNORDERED_ACCESS / non-pixel-shader-resource on
    // the dispatch access, so no explicit transition barrier is needed here (no state tracker yet).
    _list->SetComputeRootDescriptorTable(0, dg->table_start);
}

void dx12_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");
    _list->Dispatch(UINT(x), UINT(y), UINT(z));

    // Flush UAV writes so a later command list (e.g. the download copy) observes them. Buffers then
    // decay to COMMON at ExecuteCommandLists and the copy implicitly promotes them to COPY_SOURCE.
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr; // global UAV barrier
    _list->ResourceBarrier(1, &uav);
}

void dx12_command_list::upload_bytes_to_buffer(sg::buffer_handle buffer,
                                               cc::span<cc::byte const> data,
                                               cc::isize offset_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    auto const* const dst = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(dst != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= dst->size_in_bytes(), "upload range is out of "
                                                                                             "the buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(sg::has_flag(dst->usage(), sg::buffer_usage::copy_dst), "upload target buffer must have "
                                                                      "buffer_usage::copy_dst");
    _ctx._upload_inline.upload_buffer(*this, *dst, data, offset_in_bytes);
}

sg::bytes_future dx12_command_list::download_bytes_from_buffer(sg::buffer_handle buffer,
                                                               cc::isize offset_in_bytes,
                                                               cc::isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    auto const* const src = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(size_in_bytes >= 0, "download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= src->size_in_bytes(),
              "download range is out of the buffer's bounds");
    if (size_in_bytes > 0)
        CC_ASSERT(sg::has_flag(src->usage(), sg::buffer_usage::copy_src), "download source buffer must have "
                                                                          "buffer_usage::copy_src");
    return _ctx._download_inline.download_buffer(*this, *src, offset_in_bytes, size_in_bytes);
}

void dx12_command_list::copy_buffer_region(sg::buffer_handle src,
                                           sg::buffer_handle dst,
                                           cc::isize src_offset_in_bytes,
                                           cc::isize dst_offset_in_bytes,
                                           cc::isize size_in_bytes)
{
    CC_ASSERT(src != nullptr, "copy source buffer is null");
    CC_ASSERT(dst != nullptr, "copy dest buffer is null");
    auto const* const s = dynamic_cast<dx12_buffer const*>(src.get());
    auto const* const d = dynamic_cast<dx12_buffer const*>(dst.get());
    CC_ASSERT(s != nullptr && d != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && src_offset_in_bytes + size_in_bytes <= s->size_in_bytes(),
              "copy source range is out of the buffer's bounds");
    CC_ASSERT(dst_offset_in_bytes >= 0 && dst_offset_in_bytes + size_in_bytes <= d->size_in_bytes(),
              "copy dest range is out of the buffer's bounds");
    if (size_in_bytes == 0)
        return;
    CC_ASSERT(sg::has_flag(s->usage(), sg::buffer_usage::copy_src), "copy source buffer must have "
                                                                    "buffer_usage::copy_src");
    CC_ASSERT(sg::has_flag(d->usage(), sg::buffer_usage::copy_dst), "copy dest buffer must have "
                                                                    "buffer_usage::copy_dst");
    // Same-buffer copy: the source and destination ranges must not overlap.
    if (s->_resource.Get() == d->_resource.Get())
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");

    // Conservative global barrier for correctness: prior GPU writes (e.g. a UAV write into the source)
    // must be visible before the copy reads them. Coarse and heavy-handed.
    // TODO: replace with granular per-resource transition barriers once the state-tracking barrier
    // system lands (it will emit the precise COPY_SOURCE/COPY_DEST transitions this stands in for).
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr; // null resource = global barrier over all UAV accesses
    _list->ResourceBarrier(1, &barrier);

    _list->CopyBufferRegion(d->_resource.Get(), UINT64(dst_offset_in_bytes), s->_resource.Get(),
                            UINT64(src_offset_in_bytes), UINT64(size_in_bytes));
}

cc::result<std::unique_ptr<dx12_command_list>> dx12_context::create_dx12_command_list()
{
    // Single DIRECT queue for now; the pool is per-queue-ready for the copy/compute/video queues to come.
    auto constexpr queue = D3D12_COMMAND_LIST_TYPE_DIRECT;
    auto acquired = _cmd_pool.acquire_command_list(queue);
    CC_RETURN_IF_ERROR(acquired);

    _open_command_lists.fetch_add(1, std::memory_order_relaxed); // must reach 0 before the epoch can advance
    // Left open (recording); submit closes it. Stamped with the epoch it must be submitted/dropped in.
    return std::make_unique<dx12_command_list>(
        *this, current_epoch(), queue, cc::move(acquired.value().allocator.allocator), cc::move(acquired.value().list));
}

sg::submission_token dx12_context::submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");

    HRESULT const hr = cmd->_list->Close();
    CC_ASSERT(SUCCEEDED(hr), "ID3D12GraphicsCommandList::Close failed");

    // Execute, take a monotonic completion token, and signal it — all under one lock so token order
    // equals queue submission and signal order. (The queue is free-threaded, but out-of-order signals
    // would move the fence's completed value backwards and break is_submission_complete.)
    sg::submission_token const token = _next_submission.lock(
        [&](sg::submission_token& next)
        {
            ID3D12CommandList* lists[] = {cmd->_list.Get()};
            _queue->ExecuteCommandLists(1, lists);

            sg::submission_token const t = next;
            next = sg::submission_token(cc::u64(next) + 1);
            HRESULT const sig = _queue->Signal(_submission_fence.Get(), cc::u64(t));
            CC_ASSERT(SUCCEEDED(sig), "ID3D12CommandQueue::Signal failed");

            // Stamp this list's deferred downloads with the token and hand them to the actor under the
            // same lock, so the actor's copy order matches submission (and thus ring-allocation) order.
            _download_inline.enqueue_submitted(t, cmd->_pending_downloads);
            return t;
        });

    // The allocator is in flight until this epoch retires — hand it to the pool's epoch capture. The
    // list is already closed and can be reused now (resetting an in-flight list onto a fresh, GPU-safe
    // allocator is legal), so return it to the pool for the next acquire.
    _cmd_pool.return_command_list(cmd->_queue, cc::move(cmd->_list));
    _cmd_pool.return_submitted_allocator({cc::move(cmd->_allocator), cmd->_queue});
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
    return token;
}

void dx12_context::drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be dropped in the epoch it was opened "
                                                          "in");

    // Never submitted, so its recorded downloads will never run — reclaim their reserved readback space.
    _download_inline.discard_unsubmitted(cmd->_pending_downloads);

    // Never submitted, so the GPU never touched this allocator. Close the list so it is poolable, then
    // return both to the pool: the list for reuse, the allocator straight to the free pool (it was
    // never executed, so no epoch gates it; reset happens at reuse).
    HRESULT const closed = cmd->_list->Close();
    CC_ASSERT(SUCCEEDED(closed), "ID3D12GraphicsCommandList::Close failed");
    _cmd_pool.return_command_list(cmd->_queue, cc::move(cmd->_list));
    _cmd_pool.return_free_allocator({cc::move(cmd->_allocator), cmd->_queue});
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}
} // namespace sg::backend::dx12
