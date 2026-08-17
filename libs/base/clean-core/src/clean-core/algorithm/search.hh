#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/common/range_traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>

// Binary search over an already-ordered indexed range.
//
// The `_in_sorted` suffix is a CONTRACT MARKER, not decoration: these cost O(log n) against a precondition the
// caller must meet, and cc::sequence owns the linear scanning `find`. A bare `first_at_least` would read as one.
// Nothing here permutes, so none of it needs the index_swap_range seam — plain indexing is enough.

namespace cc::impl
{
/// partition_point over [start, start + size), factored out so the range searches can narrow the second probe.
template <class RangeT, class PredF>
[[nodiscard]] constexpr isize search_partition_point(RangeT& values, isize start, isize size, PredF& pred)
{
    while (size > 0)
    {
        isize const half = size / 2;
        isize const mid = start + half;

        if (bool(cc::invoke(pred, values[mid])))
        {
            start = mid + 1;
            size -= half + 1;
        }
        else
        {
            size = half;
        }
    }
    return start;
}
} // namespace cc::impl

namespace cc
{
/// The first index where `pred` goes false, or values.size() when it never does.
///
/// `values` must be PARTITIONED with respect to `pred`: every element satisfying it comes before every element
/// that does not.
/// That is weaker than sorted, which is why this one carries no `_in_sorted` suffix.
/// A range that is not partitioned gives an unspecified index rather than a diagnosable failure — the search
/// cannot tell the difference in O(log n).
///
///   auto const first_adult = cc::partition_point(people, [](auto const& p) { return p.age < 18; });
template <class RangeT, class PredF>
[[nodiscard]] constexpr isize partition_point(RangeT&& values, PredF&& pred)
{
    static_assert(cc::indexed_range<RangeT>, "cc::partition_point takes an indexed range");
    return impl::search_partition_point(values, 0, isize(values.size()), pred);
}

/// The first index whose element does not compare before `value`, i.e. where `value` could be inserted.
/// Returns values.size() when every element compares before it.
/// `values` must be ordered by `compare`.
template <class RangeT, class T, class CompareF = cc::default_less>
[[nodiscard]] constexpr isize first_at_least_in_sorted(RangeT&& values, T const& value, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::first_at_least_in_sorted takes an indexed range");

    auto below = [&](auto const& e) { return bool(compare(e, value)); };
    return impl::search_partition_point(values, 0, isize(values.size()), below);
}

/// The first index whose element compares strictly after `value`.
/// Returns values.size() when none does, and equals first_at_least_in_sorted when `value` is absent.
/// `values` must be ordered by `compare`.
template <class RangeT, class T, class CompareF = cc::default_less>
[[nodiscard]] constexpr isize first_greater_in_sorted(RangeT&& values, T const& value, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::first_greater_in_sorted takes an indexed range");

    auto not_above = [&](auto const& e) { return !bool(compare(value, e)); };
    return impl::search_partition_point(values, 0, isize(values.size()), not_above);
}

/// The index of an element equivalent to `value`, or nullopt.
/// Which one, when several compare equivalent, is unspecified — cc::find_range_in_sorted gives all of them.
/// `values` must be ordered by `compare`.
///
/// Returns the index rather than a bool so that finding it and using it is one search, not two.
template <class RangeT, class T, class CompareF = cc::default_less>
[[nodiscard]] constexpr cc::optional<isize> find_in_sorted(RangeT&& values, T const& value, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::find_in_sorted takes an indexed range");

    isize const size = isize(values.size());
    isize const idx = cc::first_at_least_in_sorted(values, value, compare);

    if (idx < size && !bool(compare(value, values[idx])))
        return idx;
    return cc::nullopt;
}

/// Every element equivalent to `value`, as one contiguous window.
///
/// A size of 0 means absent, and the offset is then where `value` would be inserted — so this answers
/// "is it there", "where is it" and "where does it go" in one search, which is why it never returns an optional.
/// `values` must be ordered by `compare`.
///
///   auto const window = cc::find_range_in_sorted(entries, key);
///   for (isize i = window.offset; i < window.offset + window.size; ++i) ...
template <class RangeT, class T, class CompareF = cc::default_less>
[[nodiscard]] constexpr cc::offset_size find_range_in_sorted(RangeT&& values, T const& value, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::find_range_in_sorted takes an indexed range");

    isize const size = isize(values.size());
    isize const first = cc::first_at_least_in_sorted(values, value, compare);

    // the upper bound can only lie at or after `first`, so the second probe searches the tail rather than the whole
    auto not_above = [&](auto const& e) { return !bool(compare(value, e)); };
    isize const last = impl::search_partition_point(values, first, size - first, not_above);

    return {.offset = first, .size = last - first};
}
} // namespace cc
