#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/memory_heap.hh>

/// Metal implementation of sg::memory_heap, over an MTLHeap of type placement.
///
/// A placement heap is the one Metal heap kind that lets a caller choose the offset, which is what sg's model needs:
/// the external allocator picks, and the heap only validates and mints.
/// The other kind sub-allocates for you and hands back no offset at all.
///
/// Unlike dx12, placement works for textures here from the start — the heap does not have to be told which it will
/// hold, so there is no buffers-only stage to grow out of.
class sg::backend::metal::metal_memory_heap final : public sg::memory_heap
{
public:
    metal_memory_heap(metal_context& ctx, isize size_in_bytes, MTL::Heap* heap)
      : sg::memory_heap(size_in_bytes), _ctx(ctx), _heap(heap)
    {
    }

    ~metal_memory_heap() override;

    [[nodiscard]] MTL::Heap* heap() const { return _heap; }

private:
    [[nodiscard]] sg::memory_requirements query_buffer_requirements(isize size_in_bytes,
                                                                    sg::buffer_usages usage) const override;

    metal_context& _ctx;
    MTL::Heap* _heap = nullptr;
};
