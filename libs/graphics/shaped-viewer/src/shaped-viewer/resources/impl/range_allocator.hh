#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// First-fit sub-allocation of element ranges out of one growable span `[0, capacity)`.
///
/// It hands out offsets and nothing else: what lives at them, and when a freed range may really be reused, is the caller's.
/// The free list is kept sorted and coalesced, so `allocate` and `free` are linear in the number of free ranges.
///
/// TEMPORARY home: an offset allocator is generic and belongs in clean-core once a second caller wants one.
class range_allocator
{
public:
    range_allocator() = default;
    explicit range_allocator(isize capacity) { grow(capacity); }

    /// The offset of a fresh range of `size` elements, or none when no free range is that large.
    /// `size` must be > 0.
    [[nodiscard]] cc::optional<isize> allocate(isize size);

    /// Returns `[offset, offset + size)` to the free list.
    /// The range must be one `allocate` handed out, freed once.
    void free(isize offset, isize size);

    /// Extends the span to `new_capacity`, which must be >= `capacity()`; the new tail is free.
    void grow(isize new_capacity);

    [[nodiscard]] isize capacity() const { return _capacity; }
    [[nodiscard]] isize used() const { return _used; }

    /// The size of the largest range `allocate` could hand out right now.
    [[nodiscard]] isize largest_free() const;

private:
    struct free_range
    {
        isize offset = 0;
        isize size = 0;
    };

    /// sorted by offset; no two ranges touch, since `free` merges them
    cc::vector<free_range> _free;
    isize _capacity = 0;
    isize _used = 0;
};
} // namespace sv::impl
