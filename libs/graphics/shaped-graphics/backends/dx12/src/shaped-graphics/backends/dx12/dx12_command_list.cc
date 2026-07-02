// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here. Allocators are epoch-gated (recycled once the
// epoch retires); see libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
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

    // Never submitted, so the GPU never touched this allocator — release the list object, then
    // return the allocator straight to the free pool (reset happens at reuse). Teardown lives in the dtor.
    cmd->_list.Reset();
    _allocators.lock([&](dx12_allocator_pool& p) { p.free.push_back(cc::move(cmd->_allocator)); });
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}
} // namespace sg::backend::dx12
