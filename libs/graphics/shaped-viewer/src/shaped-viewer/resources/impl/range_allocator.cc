#include <clean-core/common/assert.hh>
#include <shaped-viewer/resources/impl/range_allocator.hh>

namespace sv::impl
{
cc::optional<isize> range_allocator::allocate(isize size)
{
    CC_ASSERT(size > 0, "a range must hold at least one element");
    for (auto i = isize(0); i < _free.size(); ++i)
    {
        auto& r = _free[i];
        if (r.size < size)
            continue;
        auto const offset = r.offset;
        r.offset += size;
        r.size -= size;
        if (r.size == 0)
            _free.remove_at(i);
        _used += size;
        return offset;
    }
    return {};
}

void range_allocator::free(isize offset, isize size)
{
    CC_ASSERT(size > 0 && offset >= 0 && offset + size <= _capacity, "freeing a range outside the span");
    CC_ASSERT(size <= _used, "freeing more than was allocated");

    // The first free range past `offset`; the freed one goes right before it.
    auto i = isize(0);
    while (i < _free.size() && _free[i].offset < offset)
        ++i;
    CC_ASSERT(i == _free.size() || offset + size <= _free[i].offset, "freeing a range that overlaps a free one");
    CC_ASSERT(i == 0 || _free[i - 1].offset + _free[i - 1].size <= offset, "freeing a range that overlaps a free one");

    _used -= size;
    auto const touches_previous = i > 0 && _free[i - 1].offset + _free[i - 1].size == offset;
    auto const touches_next = i < _free.size() && offset + size == _free[i].offset;
    if (touches_previous && touches_next)
    {
        _free[i - 1].size += size + _free[i].size;
        _free.remove_at(i);
    }
    else if (touches_previous)
        _free[i - 1].size += size;
    else if (touches_next)
    {
        _free[i].offset = offset;
        _free[i].size += size;
    }
    else
        _free.insert_at(i, free_range{.offset = offset, .size = size});
}

void range_allocator::grow(isize new_capacity)
{
    CC_ASSERT(new_capacity >= _capacity, "a span never shrinks");
    if (new_capacity == _capacity)
        return;
    auto const added = new_capacity - _capacity;
    if (!_free.empty() && _free.back().offset + _free.back().size == _capacity)
        _free.back().size += added;
    else
        _free.push_back(free_range{.offset = _capacity, .size = added});
    _capacity = new_capacity;
}

isize range_allocator::largest_free() const
{
    auto largest = isize(0);
    for (auto const& r : _free)
        largest = r.size > largest ? r.size : largest;
    return largest;
}
} // namespace sv::impl
