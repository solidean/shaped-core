#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/common/range_traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

// The seam every algorithm in this topic is written against, plus the operations that need nothing more than it.
// Sorting builds on this in clean-core/algorithm/sort.hh; searching reads through it in clean-core/algorithm/search.hh.

namespace cc
{
/// A "virtual" range the permuting algorithms read from and permute, addressed by index.
///
/// Permuting here never parks an element in a temporary: it only ever reads one and swaps two.
/// That is what lets one call keep several parallel ranges in step, which no move-based algorithm can do.
///
/// Deliberately not modelled by containers — cc::vector is not an index_swap_range, an adapter over it is:
///   auto const range = cc::as_index_swap_range(values);
///
/// element_get(i) may return a reference or a computed value.
/// element_swap(a, b) is never called with a == b, and permutes every range the adapter covers.
/// Neither takes the subrange being worked on: that changes constantly, and travels as separate arguments.
template <class R>
concept index_swap_range = requires(R r, isize i) {
    r.element_get(i);
    r.element_swap(i, i);
};
} // namespace cc

/// Adapter over one indexed range: reads its elements and swaps them in place.
template <class RangeT>
struct cc::index_swap_range_of
{
    RangeT* values = nullptr;

    constexpr decltype(auto) element_get(isize i) const { return (*values)[i]; }
    constexpr void element_swap(isize a, isize b) const { cc::swap((*values)[a], (*values)[b]); }
};

/// Adapter that orders one indexed range by a key derived from each element.
/// The key is recomputed on every comparison — for an expensive one reach for cc::sort_by_cached_key.
template <class RangeT, class KeyF>
struct cc::index_swap_range_by
{
    RangeT* values = nullptr;
    KeyF* key = nullptr;

    constexpr decltype(auto) element_get(isize i) const { return cc::invoke(*key, (*values)[i]); }
    constexpr void element_swap(isize a, isize b) const { cc::swap((*values)[a], (*values)[b]); }
};

/// Adapter over several parallel ranges: compares the keys, swaps all of them.
/// Every range must have the same size, which the callers assert.
template <class KeyRangeT, class... RangeTs>
struct cc::index_swap_range_multi
{
    KeyRangeT* keys = nullptr;
    cc::tuple<RangeTs*...> values;

    constexpr decltype(auto) element_get(isize i) const { return (*keys)[i]; }
    constexpr void element_swap(isize a, isize b) const
    {
        cc::swap((*keys)[a], (*keys)[b]);
        cc::apply([&](auto*... v) { (cc::swap((*v)[a], (*v)[b]), ...); }, values);
    }
};

/// Adapter over several parallel ranges whose key is computed from all of them at once.
template <class KeyF, class... RangeTs>
struct cc::index_swap_range_multi_by
{
    KeyF* key = nullptr;
    cc::tuple<RangeTs*...> values;

    constexpr decltype(auto) element_get(isize i) const
    {
        return cc::apply([&](auto*... v) -> decltype(auto) { return cc::invoke(*key, (*v)[i]...); }, values);
    }
    constexpr void element_swap(isize a, isize b) const
    {
        cc::apply([&](auto*... v) { (cc::swap((*v)[a], (*v)[b]), ...); }, values);
    }
};

namespace cc
{
// =========================================================================================================
// Adapter factories
// =========================================================================================================
// Each borrows what it is given, so the adapter must not outlive the ranges, nor the key or comparator.

template <class RangeT>
[[nodiscard]] constexpr auto as_index_swap_range(RangeT& values)
{
    return cc::index_swap_range_of<RangeT>{.values = &values};
}

template <class RangeT, class KeyF>
[[nodiscard]] constexpr auto as_index_swap_range_by(RangeT& values, KeyF& key)
{
    return cc::index_swap_range_by<RangeT, KeyF>{.values = &values, .key = &key};
}

template <class KeyRangeT, class... RangeTs>
[[nodiscard]] constexpr auto as_index_swap_range_multi(KeyRangeT& keys, RangeTs&... values)
{
    return cc::index_swap_range_multi<KeyRangeT, RangeTs...>{.keys = &keys, .values = cc::tuple<RangeTs*...>(&values...)};
}

template <class KeyF, class... RangeTs>
[[nodiscard]] constexpr auto as_index_swap_range_multi_by(KeyF& key, RangeTs&... values)
{
    return cc::index_swap_range_multi_by<KeyF, RangeTs...>{.key = &key, .values = cc::tuple<RangeTs*...>(&values...)};
}

namespace impl
{
/// Lifts an element predicate to an index predicate, reading through the range being permuted.
template <class RangeT, class PredF>
struct index_swap_element_predicate
{
    RangeT range;
    PredF* pred;

    constexpr bool operator()(isize i) const { return bool(cc::invoke(*pred, range.element_get(i))); }
};
} // namespace impl

// =========================================================================================================
// Partitioning
// =========================================================================================================

/// Permutes [start, start + size) into a left block where `is_right(i)` is false and a right block where it is true.
///
/// Returns the first index of the right block: start + size when nothing is right, start when everything is.
/// `is_right(i)` takes an INDEX, so it usually reads through the same range it permutes.
/// Runs in O(size) and is not stable.
template <class IsRightF, class RangeT>
constexpr isize partition_ex(isize start, isize size, IsRightF&& is_right, RangeT range)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::partition_ex takes an index_swap_range — see "
                                                "cc::as_index_swap_range");
    CC_ASSERT(size >= 0, "size must be >= 0");

    isize const end = start + size;
    isize first = start;
    isize last = end - 1;

    while (true)
    {
        while (first < end && !bool(cc::invoke(is_right, first)))
            ++first;
        while (last > start && bool(cc::invoke(is_right, last)))
            --last;

        if (first >= last)
            return first;

        range.element_swap(first, last);
        ++first;
        --last;
    }
}

/// Permutes `values` into a left block where `is_right(element)` is false and a right block where it is true.
/// Returns the first index of the right block, i.e. the size of the left one.
/// Runs in O(n) and is not stable.
template <class RangeT, class IsRightF>
constexpr isize partition_by(RangeT&& values, IsRightF&& is_right)
{
    static_assert(cc::indexed_range<RangeT>, "cc::partition_by takes an indexed range");

    auto const range = cc::as_index_swap_range(values);
    auto predicate
        = impl::index_swap_element_predicate<decltype(range), std::remove_reference_t<IsRightF>>{.range = range,
                                                                                                 .pred = &is_right};
    return cc::partition_ex(0, isize(values.size()), predicate, range);
}

// =========================================================================================================
// Orderedness queries
// =========================================================================================================

/// True when [start, start + size) is ordered by `compare`, i.e. no element compares before its predecessor.
template <class RangeT, class CompareF>
[[nodiscard]] constexpr bool is_sorted_ex(isize start, isize size, RangeT range, CompareF&& compare)
{
    for (isize i = start + 1; i < start + size; ++i)
        if (compare(range.element_get(i), range.element_get(i - 1)))
            return false;
    return true;
}

/// True when [start, start + size) is ordered by `compare` with no two elements equivalent.
/// The subtle difference to is_sorted_ex: every adjacent pair must actually compare less, not merely not-greater.
template <class RangeT, class CompareF>
[[nodiscard]] constexpr bool is_strictly_sorted_ex(isize start, isize size, RangeT range, CompareF&& compare)
{
    for (isize i = start + 1; i < start + size; ++i)
        if (!compare(range.element_get(i - 1), range.element_get(i)))
            return false;
    return true;
}

/// True when `values` is ordered by `compare`. Runs in O(n).
template <class RangeT, class CompareF = cc::default_less>
[[nodiscard]] constexpr bool is_sorted(RangeT&& values, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::is_sorted takes an indexed range");
    return cc::is_sorted_ex(0, isize(values.size()), cc::as_index_swap_range(values), compare);
}

template <class RangeT, class KeyF, class CompareF = cc::default_less>
[[nodiscard]] constexpr bool is_sorted_by(RangeT&& values, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::is_sorted_by takes an indexed range");
    return cc::is_sorted_ex(0, isize(values.size()), cc::as_index_swap_range_by(values, key), compare);
}

/// True when `values` is ordered by `compare` and holds no two equivalent elements.
template <class RangeT, class CompareF = cc::default_less>
[[nodiscard]] constexpr bool is_strictly_sorted(RangeT&& values, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::is_strictly_sorted takes an indexed range");
    return cc::is_strictly_sorted_ex(0, isize(values.size()), cc::as_index_swap_range(values), compare);
}

template <class RangeT, class KeyF, class CompareF = cc::default_less>
[[nodiscard]] constexpr bool is_strictly_sorted_by(RangeT&& values, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::is_strictly_sorted_by takes an indexed range");
    return cc::is_strictly_sorted_ex(0, isize(values.size()), cc::as_index_swap_range_by(values, key), compare);
}
} // namespace cc
