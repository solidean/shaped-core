// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here. Allocators are epoch-gated (recycled once the
// epoch retires); see libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
void dx12_command_list::upload_to_buffer(sg::buffer_handle buffer, cc::span<cc::byte const> data, cc::isize offset_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    auto* const dst = dynamic_cast<dx12_buffer*>(buffer.get());
    CC_ASSERT(dst != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= dst->size_in_bytes(), "upload range is out of "
                                                                                             "the buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(sg::has_flag(dst->usage(), sg::buffer_usage::copy_dst), "upload target buffer must have "
                                                                      "buffer_usage::copy_dst");
    _ctx._upload_inline.upload_buffer(*this, *dst, data, offset_in_bytes);
}

sg::bytes_future dx12_command_list::download_from_buffer(sg::buffer_handle buffer,
                                                         cc::isize offset_in_bytes,
                                                         cc::isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    auto* const src = dynamic_cast<dx12_buffer*>(buffer.get());
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(size_in_bytes >= 0, "download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= src->size_in_bytes(),
              "download range is out of the buffer's bounds");
    if (size_in_bytes > 0)
        CC_ASSERT(sg::has_flag(src->usage(), sg::buffer_usage::copy_src), "download source buffer must have "
                                                                          "buffer_usage::copy_src");
    return _ctx._download_inline.download_buffer(*this, *src, offset_in_bytes, size_in_bytes);
}

cc::result<std::unique_ptr<dx12_command_list>> dx12_context::create_dx12_command_list()
{
    // Reuse a pooled allocator if one is free (it re-entered the pool only once idle), else make one.
    ComPtr<ID3D12CommandAllocator> allocator = _allocators.lock(
        [](dx12_allocator_pool& p) -> ComPtr<ID3D12CommandAllocator>
        {
            if (p.free.empty())
                return {};
            return p.free.pop_back();
        });
    if (allocator)
    {
        if (HRESULT hr = allocator->Reset(); FAILED(hr))
            return dx12_error(hr, "ID3D12CommandAllocator::Reset failed");
    }
    else if (HRESULT hr = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
             FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr
        = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandList failed");

    _open_command_lists.fetch_add(1, std::memory_order_relaxed); // must reach 0 before the epoch can advance
    // Left open (recording); submit closes it. Stamped with the epoch it must be submitted/dropped in.
    return std::make_unique<dx12_command_list>(*this, current_epoch(), cc::move(allocator), cc::move(list));
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

    // The allocator is in flight until this epoch retires — hand it to the current epoch. The list
    // object itself can be dropped now (resetting an in-flight list object is legal).
    _allocators.lock([&](dx12_allocator_pool& p) { p.in_epoch.push_back(cc::move(cmd->_allocator)); });
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

    // Never submitted, so the GPU never touched this allocator — release the list object, then
    // return the allocator straight to the free pool (reset happens at reuse). Teardown lives in the dtor.
    cmd->_list.Reset();
    _allocators.lock([&](dx12_allocator_pool& p) { p.free.push_back(cc::move(cmd->_allocator)); });
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}
} // namespace sg::backend::dx12
