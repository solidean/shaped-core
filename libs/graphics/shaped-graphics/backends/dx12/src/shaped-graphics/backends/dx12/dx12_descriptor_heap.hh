#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// The context's single shader-visible CBV/SRV/UAV descriptor heap. binding_groups allocate a
/// contiguous range of descriptors from it and the command list binds their table into the pipeline.
///
/// Bump allocation only — ranges are never reclaimed yet, so a group's table lives until context
/// teardown. TODO: a free-list / epoch-reclaimed allocator (like the transient descriptor ring the
/// legacy system had) so long-running apps don't exhaust the heap.
struct dx12_descriptor_heap
{
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    UINT increment = 0;
    UINT capacity = 0;
    std::atomic<UINT> cursor{0};

    [[nodiscard]] cc::result<cc::unit> initialize(ID3D12Device* device, UINT descriptor_capacity)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = descriptor_capacity;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr))
            return dx12_error(hr, "CreateDescriptorHeap (shader-visible CBV/SRV/UAV) failed");

        cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
        gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        capacity = descriptor_capacity;
        return cc::unit{};
    }

    /// Bump-allocates `count` contiguous descriptors, returning the start offset.
    [[nodiscard]] UINT allocate(UINT count)
    {
        UINT const offset = cursor.fetch_add(count, std::memory_order_relaxed);
        CC_ASSERT(offset + count <= capacity, "shader-visible descriptor heap exhausted (bump allocator, no reclaim "
                                              "yet)");
        return offset;
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(UINT offset) const
    {
        return {cpu_start.ptr + SIZE_T(offset) * SIZE_T(increment)};
    }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(UINT offset) const
    {
        return {gpu_start.ptr + UINT64(offset) * UINT64(increment)};
    }
};
} // namespace sg::backend::dx12
