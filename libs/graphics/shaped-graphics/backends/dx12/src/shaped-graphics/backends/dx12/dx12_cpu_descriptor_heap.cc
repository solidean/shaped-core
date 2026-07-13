// dx12_cpu_descriptor_heap: a flat non-shader-visible RTV/DSV descriptor slab — bump cursor plus a
// free list for slot reuse. See dx12_cpu_descriptor_heap.hh.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_cpu_descriptor_heap.hh>

namespace sg::backend::dx12
{
cc::result<std::unique_ptr<dx12_cpu_descriptor_heap>> dx12_cpu_descriptor_heap::create(dx12_context& ctx,
                                                                                       D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
                                                                                       int descriptor_capacity)
{
    CC_ASSERT(descriptor_capacity > 0, "descriptor heap capacity must be positive");

    auto h = std::make_unique<dx12_cpu_descriptor_heap>();

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = heap_type;
    desc.NumDescriptors = UINT(descriptor_capacity);
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only: RTV/DSV are never shader-visible
    if (HRESULT hr = ctx._device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&h->heap)); FAILED(hr))
        return dx12_error(hr, "CreateDescriptorHeap (RTV/DSV, non-shader-visible) failed");

    h->cpu_start = h->heap->GetCPUDescriptorHandleForHeapStart();
    h->increment = int(ctx._device->GetDescriptorHandleIncrementSize(heap_type));
    h->capacity = descriptor_capacity;
    return h;
}

cpu_descriptor_slot dx12_cpu_descriptor_heap::allocate()
{
    return state.lock(
        [&](alloc_state& s) -> cpu_descriptor_slot
        {
            if (!s.recycled.empty())
            {
                int const slot = s.recycled.back();
                s.recycled.pop_back();
                return cpu_descriptor_slot(slot);
            }
            if (s.next_free < capacity)
                return cpu_descriptor_slot(s.next_free++);
            return cpu_descriptor_slot::invalid; // exhausted
        });
}

void dx12_cpu_descriptor_heap::free(cpu_descriptor_slot slot)
{
    int const index = int(slot);
    CC_ASSERT(index >= 0 && index < capacity, "freed descriptor slot out of range");
    state.lock([&](alloc_state& s) { s.recycled.push_back(index); });
}
} // namespace sg::backend::dx12
