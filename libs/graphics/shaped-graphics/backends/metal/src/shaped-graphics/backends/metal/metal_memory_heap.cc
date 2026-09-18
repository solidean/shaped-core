#include "metal_memory_heap.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_memory_heap::~metal_memory_heap()
{
    if (_heap == nullptr)
        return;

    // Deferred like every other device object: a placement inside this heap may still be in flight.
    auto* const heap = _heap;
    _heap = nullptr;
    _ctx.epochs().defer([heap] { heap->release(); });
}

sg::memory_requirements metal_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages) const
{
    // An empty buffer occupies nothing and needs no alignment, and asking Metal about a zero length is a validation
    // error rather than an answer.
    if (size_in_bytes <= 0)
        return {.alignment_in_bytes = 1, .size_in_bytes = 0};

    // Metal derives the requirement from the length and the options, not from any usage — a buffer is a buffer here,
    // where D3D12 and Vulkan both want to know what it is for.
    auto const size_and_align = _ctx.device()->heapBufferSizeAndAlign(NS::UInteger(size_in_bytes), k_buffer_options);

    auto const alignment = isize(size_and_align.align);
    CC_ASSERT(alignment > 0, "metal reported a zero buffer alignment");

    // **The reported size is rounded up to the alignment, and Metal's is not.**
    //
    // `sg::context_transient_scope`'s bump allocator advances its head by this size and never re-aligns, so the size a
    // heap reports has to already carry the padding that keeps the next placement legal.
    // D3D12 and Vulkan both round for you; Metal answers the two questions independently and returns, for a 1-byte
    // buffer, a size of 1 at an alignment of 256 — so every placement after the first would land unaligned.
    auto const size = isize(size_and_align.size);
    auto const padded = (size + alignment - 1) / alignment * alignment;

    return {.alignment_in_bytes = alignment, .size_in_bytes = padded};
}
} // namespace sg::backend::metal
