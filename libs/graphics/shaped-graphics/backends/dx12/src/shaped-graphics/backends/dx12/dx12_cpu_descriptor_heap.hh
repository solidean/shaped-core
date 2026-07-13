#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

#include <memory>

namespace sg::backend::dx12
{
/// A slot index into a dx12_cpu_descriptor_heap. Strongly typed (an enum, not a bare int) so it can't be
/// confused with a count or another index. `invalid` is the null / heap-exhausted result of allocate().
enum class cpu_descriptor_slot : int
{
    invalid = -1,
};

/// A created RTV/DSV descriptor: the CPU handle to bind (OMSetRenderTargets / Clear*) plus the heap slot
/// to return via the owning context's free_* when the RTV/DSV is no longer needed.
struct dx12_descriptor_ref
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    cpu_descriptor_slot slot = cpu_descriptor_slot::invalid;
};

/// A small non-shader-visible CPU descriptor heap for RTV / DSV descriptors. Unlike dx12_descriptor_heap
/// (a shader-visible CBV/SRV/UAV heap with a GPU handle and epoch-scoped ring), RTV/DSV descriptors are
/// CPU-only — they are passed to OMSetRenderTargets / Clear* by CPU handle, never bound as a table — so
/// this is just a flat slab with a bump cursor plus a free list for slot reuse. Single-descriptor
/// allocations; no lifetime/epoch tracking (a descriptor is valid until its slot is freed or overwritten).
struct dx12_cpu_descriptor_heap
{
    /// Creates a non-shader-visible heap of `heap_type` (RTV or DSV) holding `capacity` descriptors, owned
    /// by the returned unique_ptr. Body in dx12_cpu_descriptor_heap.cc.
    [[nodiscard]] static cc::result<std::unique_ptr<dx12_cpu_descriptor_heap>> create(dx12_context& ctx,
                                                                                      D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
                                                                                      int capacity);

    /// Reserves one descriptor slot, reusing a freed slot when available. Returns cpu_descriptor_slot::invalid
    /// when the heap is exhausted (a recoverable failure the caller reports as an error).
    [[nodiscard]] cpu_descriptor_slot allocate();

    /// Returns a slot (from allocate) to the free list for reuse.
    void free(cpu_descriptor_slot slot);

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(cpu_descriptor_slot slot) const
    {
        return {cpu_start.ptr + SIZE_T(int(slot)) * SIZE_T(increment)};
    }

    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    int increment = 0;
    int capacity = 0;

    struct alloc_state
    {
        int next_free = 0;        // bump cursor over unused slots
        cc::vector<int> recycled; // freed slots available for reuse
    };
    cc::mutex<alloc_state> state;
};
} // namespace sg::backend::dx12
