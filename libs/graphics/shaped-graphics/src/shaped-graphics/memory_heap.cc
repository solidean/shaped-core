#include <clean-core/common/assert.hh>
#include <shaped-graphics/memory_heap.hh>

namespace sg
{
memory_heap::~memory_heap() = default;

memory_heap::memory_heap(isize size_in_bytes) : _size_in_bytes(size_in_bytes)
{
    // Zero is allowed — an empty heap holds no placements. Only a negative size is programmer misuse.
    CC_ASSERT(size_in_bytes >= 0, "memory heap size must be non-negative");
}

memory_requirements memory_heap::memory_requirements_for_buffer(isize size_in_bytes, buffer_usage usage) const
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    return query_buffer_requirements(size_in_bytes, usage);
}

allocation_info memory_heap::acquire_allocation_for_buffer(isize size_in_bytes, buffer_usage usage, isize offset) const
{
    memory_requirements const reqs = memory_requirements_for_buffer(size_in_bytes, usage);
    CC_ASSERT(reqs.alignment_in_bytes > 0, "backend must report a positive alignment");
    CC_ASSERT(offset >= 0 && offset % reqs.alignment_in_bytes == 0, "offset must be aligned for the resource");
    CC_ASSERT(offset + reqs.size_in_bytes <= _size_in_bytes, "allocation must lie within the heap");

    allocation_info info;
    info.heap = shared_from_this();
    info.offset = offset;
    info.size_in_bytes = reqs.size_in_bytes; // backend-reported size (may exceed the requested size)
    info.scope = allocation_scope::persistent;
    return info;
}
} // namespace sg
