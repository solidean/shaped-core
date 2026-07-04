// dx12_command_allocator_pool: pooling + reuse of command allocators (epoch-gated) and command lists
// (reused immediately). See libs/graphics/shaped-graphics/docs/concepts/epochs.md.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
cc::result<dx12_acquired_command_list> dx12_command_allocator_pool::acquire_command_list(D3D12_COMMAND_LIST_TYPE queue)
{
    CC_ASSERT(int(queue) >= 0 && int(queue) < queue_count, "invalid queue type");

    // Pop a free allocator and a free list under one lock — either may be absent.
    struct popped
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };
    popped p = _by_queue[int(queue)].lock(
        [](per_queue_pool& q)
        {
            popped out;
            if (!q.free_allocators.empty())
                out.allocator = q.free_allocators.pop_back();
            if (!q.free_lists.empty())
                out.list = q.free_lists.pop_back();
            return out;
        });

    // Reuse the pooled allocator (it re-entered the pool only once idle) or create a fresh one.
    if (p.allocator)
    {
        if (HRESULT hr = p.allocator->Reset(); FAILED(hr))
            return dx12_error(hr, "ID3D12CommandAllocator::Reset failed");
    }
    else if (HRESULT hr = _ctx._device->CreateCommandAllocator(queue, IID_PPV_ARGS(&p.allocator)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandAllocator failed");

    // Reuse a pooled list (cheap reset onto the allocator) or create one (expensive). Left recording.
    if (p.list)
    {
        if (HRESULT hr = p.list->Reset(p.allocator.Get(), nullptr); FAILED(hr))
            return dx12_error(hr, "ID3D12GraphicsCommandList::Reset failed");
    }
    else if (HRESULT hr = _ctx._device->CreateCommandList(0, queue, p.allocator.Get(), nullptr, IID_PPV_ARGS(&p.list));
             FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandList failed");

    return dx12_acquired_command_list{.allocator = {cc::move(p.allocator), queue}, .list = cc::move(p.list)};
}

void dx12_command_allocator_pool::return_submitted_allocator(dx12_pooled_allocator allocator)
{
    CC_ASSERT(allocator.allocator != nullptr, "null allocator");
    int const q = int(allocator.queue);
    CC_ASSERT(q >= 0 && q < queue_count, "invalid queue type");
    _by_queue[q].lock([&](per_queue_pool& p) { p.in_epoch.push_back(cc::move(allocator.allocator)); });
}

void dx12_command_allocator_pool::return_free_allocator(dx12_pooled_allocator allocator)
{
    CC_ASSERT(allocator.allocator != nullptr, "null allocator");
    int const q = int(allocator.queue);
    CC_ASSERT(q >= 0 && q < queue_count, "invalid queue type");
    _by_queue[q].lock([&](per_queue_pool& p) { p.free_allocators.push_back(cc::move(allocator.allocator)); });
}

void dx12_command_allocator_pool::return_command_list(D3D12_COMMAND_LIST_TYPE queue,
                                                      ComPtr<ID3D12GraphicsCommandList> list)
{
    CC_ASSERT(list != nullptr, "null command list");
    int const q = int(queue);
    CC_ASSERT(q >= 0 && q < queue_count, "invalid queue type");
    _by_queue[q].lock([&](per_queue_pool& p) { p.free_lists.push_back(cc::move(list)); });
}

cc::vector<dx12_pooled_allocator> dx12_command_allocator_pool::drain_in_epoch_allocators()
{
    cc::vector<dx12_pooled_allocator> out;
    for (int q = 0; q < queue_count; ++q)
        _by_queue[q].lock(
            [&](per_queue_pool& p)
            {
                for (auto& a : p.in_epoch)
                    out.push_back({cc::move(a), D3D12_COMMAND_LIST_TYPE(q)});
                p.in_epoch.clear();
            });
    return out;
}

void dx12_command_allocator_pool::reclaim_allocators(cc::vector<dx12_pooled_allocator> allocators)
{
    for (auto& a : allocators)
    {
        CC_ASSERT(a.allocator != nullptr, "null allocator");
        a.allocator->Reset(); // safe now: every list sourced from it has finished on the GPU
        _by_queue[int(a.queue)].lock([&](per_queue_pool& p) { p.free_allocators.push_back(cc::move(a.allocator)); });
    }
}

void dx12_command_allocator_pool::shutdown()
{
    for (int q = 0; q < queue_count; ++q)
        _by_queue[q].lock(
            [](per_queue_pool& p)
            {
                p.free_allocators = {};
                p.in_epoch = {};
                p.free_lists = {};
            });
}

cc::isize dx12_command_allocator_pool::free_allocator_count(D3D12_COMMAND_LIST_TYPE queue)
{
    CC_ASSERT(int(queue) >= 0 && int(queue) < queue_count, "invalid queue type");
    return _by_queue[int(queue)].lock([](per_queue_pool& p) { return p.free_allocators.size(); });
}

cc::isize dx12_command_allocator_pool::free_command_list_count(D3D12_COMMAND_LIST_TYPE queue)
{
    CC_ASSERT(int(queue) >= 0 && int(queue) < queue_count, "invalid queue type");
    return _by_queue[int(queue)].lock([](per_queue_pool& p) { return p.free_lists.size(); });
}
} // namespace sg::backend::dx12
