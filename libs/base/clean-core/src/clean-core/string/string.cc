#include "string.hh"

#include <cstring>

// Out-of-line implementations for heap-related operations
// These are kept in the .cc to reduce header bloat and improve compile times

void cc::string::initialize_heap_from_data(char const* str, isize const len, memory_resource const* const resource)
{
    // Construct data_heap in place using placement new
    new (cc::placement_new, &_data.heap) data_heap();

    // Don't alloc for zero length
    if (len == 0)
        return;

    // Allocate storage aligned to cache line boundary
    auto const byte_size = cc::align_up(len, data_heap::alloc_alignment);
    auto alloc = cc::allocation<char>::create_empty_bytes(byte_size, byte_size, data_heap::alloc_alignment, resource);

    // Copy the string data into the allocation
    std::memcpy(alloc.obj_start, str, size_t(len));
    alloc.obj_end = alloc.obj_start + len;

    // Move the allocation into the heap wrapper
    // Access via extract_allocation() to avoid private member access
    _data.heap = data_heap::create_from_allocation(cc::move(alloc));
}

void cc::string::materialize_heap(isize const min_back_capacity)
{
    CC_ASSERT(is_small(), "already heap");

    // Save small string state before overwriting the union
    auto const small_sz = _data.small.size;
    auto const res = remove_small_tag(_data.small.custom_resource);
    auto const data_copy = _data.blocks;

    // Construct data_heap in place
    new (cc::placement_new, &_data.heap) data_heap();

    // Allocate with room for small string content plus requested capacity
    auto const byte_size = cc::align_up(small_sz + min_back_capacity, data_heap::alloc_alignment);
    auto alloc = cc::allocation<char>::create_empty_bytes(byte_size, byte_size, data_heap::alloc_alignment, res);

    // Copy small string data into the allocation (unrolled, fits in 64B alignment)
    static_assert(data_heap::alloc_alignment >= 64);
    auto* dest = reinterpret_cast<u64*>(alloc.obj_start);
    dest[0] = data_copy.blocks[0];
    dest[1] = data_copy.blocks[1];
    dest[2] = data_copy.blocks[2];
    dest[3] = data_copy.blocks[3];
    dest[4] = data_copy.blocks[4];
    alloc.obj_end = alloc.obj_start + small_sz;

    // Move the allocation into the heap wrapper
    _data.heap = data_heap::create_from_allocation(cc::move(alloc));
}
