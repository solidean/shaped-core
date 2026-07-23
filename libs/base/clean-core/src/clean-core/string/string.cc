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
    auto const byte_size = cc::align_up(len, data_heap::alloc_alignment());
    auto alloc = cc::allocation<char>::create_empty_bytes(byte_size, byte_size, data_heap::alloc_alignment(), resource);

    // Copy the string data into the allocation
    std::memcpy(alloc.obj_start, str, size_t(len));
    alloc.obj_end = alloc.obj_start + len;

    // Move the allocation into the heap wrapper
    // Access via extract_allocation() to avoid private member access
    _data.heap = data_heap::create_from_allocation(cc::move(alloc));
}

cc::isize cc::string::replace_all(char const from, char const to)
{
    auto* const d = data();
    auto const n = size();
    isize count = 0;
    for (isize i = 0; i < n; ++i)
    {
        if (d[i] == from)
        {
            d[i] = to;
            ++count;
        }
    }
    return count;
}

cc::isize cc::string::replace_all(string_view const from, string_view const to)
{
    if (from.empty())
        return 0;

    string_view const self(*this);
    isize at = self.find(from);
    if (at < 0)
        return 0;

    // self references the current buffer; the rebuilt result is a separate allocation,
    // so it is safe even if `to` aliases this string (we only move at the very end).
    auto result = create_with_capacity(size(), resource());
    isize pos = 0;
    isize count = 0;
    while (at >= 0)
    {
        result.append(self.subview({.start = pos, .end = at}));
        result.append(to);
        pos = at + from.size();
        ++count;
        at = self.find(from, pos);
    }
    result.append(self.subview(pos));

    *this = cc::move(result);
    return count;
}

bool cc::string::replace_first(char const from, char const to)
{
    auto const i = find(from);
    if (i < 0)
        return false;
    data()[i] = to;
    return true;
}

bool cc::string::replace_first(string_view const from, string_view const to)
{
    if (from.empty())
        return false;
    auto const i = find(from);
    if (i < 0)
        return false;
    replace(offset_size{.offset = i, .size = from.size()}, to);
    return true;
}

bool cc::string::replace_last(char const from, char const to)
{
    auto const i = rfind(from);
    if (i < 0)
        return false;
    data()[i] = to;
    return true;
}

bool cc::string::replace_last(string_view const from, string_view const to)
{
    if (from.empty())
        return false;
    auto const i = rfind(from);
    if (i < 0)
        return false;
    replace(offset_size{.offset = i, .size = from.size()}, to);
    return true;
}

void cc::string::replace(offset_size const r, string_view const with)
{
    CC_ASSERT(r.size >= 0, "replace size must be non-negative");
    CC_ASSERT(r.offset >= 0 && r.offset + r.size <= size(), "replace range out of bounds");

    // self and `with` may reference this string's buffer; the rebuilt result is a
    // separate allocation and we only move into *this at the end, so aliasing is safe.
    string_view const self(*this);
    auto result = create_with_capacity(size() - r.size + with.size(), resource());
    result.append(self.subview({.start = isize(0), .end = r.offset}));
    result.append(with);
    result.append(self.subview(r.offset + r.size));

    *this = cc::move(result);
}

void cc::string::replace(start_end const r, string_view const with)
{
    CC_ASSERT(r.end >= r.start, "replace end must not precede start");
    replace(offset_size{.offset = r.start, .size = r.end - r.start}, with);
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
    auto const byte_size = cc::align_up(small_sz + min_back_capacity, data_heap::alloc_alignment());
    auto alloc = cc::allocation<char>::create_empty_bytes(byte_size, byte_size, data_heap::alloc_alignment(), res);

    // Copy the inline small-string bytes into the fresh allocation. The destination spans at least
    // alloc_alignment (>= 64) bytes, so copying the whole inline buffer is always in bounds — and reading
    // small_capacity bytes from the saved union stays within data_blocks on any pointer size.
    static_assert(data_heap::alloc_alignment() >= 64);
    std::memcpy(alloc.obj_start, &data_copy, small_capacity);
    alloc.obj_end = alloc.obj_start + small_sz;

    // Move the allocation into the heap wrapper
    _data.heap = data_heap::create_from_allocation(cc::move(alloc));
}

void cc::string::materialize_heap_front(isize const front_capacity, isize const back_capacity)
{
    CC_ASSERT(is_small(), "already heap");
    CC_ASSERT(front_capacity >= 0 && back_capacity >= 0, "capacities must be non-negative");

    // Save small string state before overwriting the union
    auto const small_sz = _data.small.size;
    auto const res = remove_small_tag(_data.small.custom_resource);
    auto const data_copy = _data.blocks;

    // Construct data_heap in place
    new (cc::placement_new, &_data.heap) data_heap();

    // Total capacity = front slack + content + back slack; the content starts at offset front_capacity.
    auto const byte_size = cc::align_up(front_capacity + small_sz + back_capacity, data_heap::alloc_alignment());
    auto alloc = cc::allocation<char>::create_empty_bytes(byte_size, byte_size, data_heap::alloc_alignment(), res,
                                                          front_capacity);

    // Copy only the live bytes, not a fixed small_capacity block: at offset front_capacity a full-width copy could run past alloc_end.
    // small_sz bytes always fit — [front_capacity, front_capacity + small_sz) lies within the allocation.
    std::memcpy(alloc.obj_start, &data_copy, size_t(small_sz));
    alloc.obj_end = alloc.obj_start + small_sz;

    // Move the allocation into the heap wrapper
    _data.heap = data_heap::create_from_allocation(cc::move(alloc));
}

void cc::string::demote_to_small()
{
    CC_ASSERT(!is_small(), "already small");
    CC_ASSERT(size() <= small_capacity, "content does not fit inline");

    // Save content and resource before tearing down the heap union.
    // resource() reads the tagged pointer member, which aliases the allocation's custom_resource here;
    // in heap mode the tag bit is clear, so remove_small_tag is a no-op and returns the real resource.
    auto const sz = _data.heap.size();
    auto const res = resource();
    char buf[small_capacity];
    std::memcpy(buf, _data.heap.data(), size_t(sz));

    // Free the heap allocation, then re-establish small mode and restore the bytes.
    _data.heap.~data_heap();
    initialize_small_empty(res);
    std::memcpy(_data.small.data, buf, size_t(sz));
    _data.small.size = u8(sz);
}
