#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_query.hh>

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] D3D12_QUERY_HEAP_TYPE to_d3d12_heap_type(dx12_query_heap_type type)
{
    switch (type)
    {
    case dx12_query_heap_type::timestamp:
        return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    case dx12_query_heap_type::count:
        break;
    }
    CC_UNREACHABLE("unsupported query heap type");
}
} // namespace

cc::result<cc::unit> dx12_query_system::initialize()
{
    // The direct queue's timestamp frequency is in ticks per second, so the tick→seconds factor is its
    // reciprocal. A failure here just means timestamps are unsupported on this device/queue.
    cc::u64 freq = 0;
    if (HRESULT hr = _ctx._queue->GetTimestampFrequency(&freq); FAILED(hr) || freq == 0)
        return dx12_error(FAILED(hr) ? hr : E_FAIL, "ID3D12CommandQueue::GetTimestampFrequency failed");

    _timestamp_tick_to_seconds = 1.0 / double(freq);
    _supports_timestamps = true;
    return cc::unit{};
}

ComPtr<ID3D12QueryHeap> dx12_query_system::create_heap(dx12_query_heap_type type)
{
    D3D12_QUERY_HEAP_DESC const desc = {
        .Type = to_d3d12_heap_type(type),
        .Count = UINT(SlotsPerHeap),
        .NodeMask = 0,
    };

    ComPtr<ID3D12QueryHeap> heap;
    [[maybe_unused]] HRESULT const hr = _ctx._device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
    CC_ASSERT(SUCCEEDED(hr) && heap, "ID3D12Device::CreateQueryHeap failed");
    return heap;
}

cc::unique_ptr<dx12_query_heap_lease> dx12_query_system::acquire_heap(dx12_query_heap_type type)
{
    CC_ASSERT(int(type) < int(dx12_query_heap_type::count), "invalid query heap type");

    cc::unique_ptr<dx12_query_heap_lease> lease;
    _free_list_by_type[int(type)].lock(
        [&](cc::vector<cc::unique_ptr<dx12_query_heap_lease>>& list)
        {
            if (!list.empty())
            {
                lease = cc::move(list.back());
                list.pop_back();
            }
        });

    if (lease == nullptr)
    {
        lease = cc::make_unique<dx12_query_heap_lease>();
        lease->heap = create_heap(type);
        lease->type = type;
        lease->slot_count = SlotsPerHeap;
    }

    CC_ASSERT(lease->next_slot == 0, "leased query heap should have next_slot == 0");
    CC_ASSERT(lease->type == type, "leased query heap has the wrong type");
    return lease;
}

void dx12_query_system::release_heap(cc::unique_ptr<dx12_query_heap_lease> lease)
{
    if (lease == nullptr)
        return;

    // Reset the bump cursor and install a fresh invalid future for the next leaseholder. Handles from the
    // previous lease keep their own shared_future (already assigned its real readback at submit).
    lease->next_slot = 0;
    lease->shared_future = std::make_shared<sg::data_future<cc::u64>>();

    auto const type = lease->type;
    _free_list_by_type[int(type)].lock([&](cc::vector<cc::unique_ptr<dx12_query_heap_lease>>& list)
                                       { list.push_back(cc::move(lease)); });
}

void dx12_query_system::shutdown()
{
    for (auto& free_list : _free_list_by_type)
        free_list.lock([](cc::vector<cc::unique_ptr<dx12_query_heap_lease>>& list) { list.clear(); });
}
} // namespace sg::backend::dx12
