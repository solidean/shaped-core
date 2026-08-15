#pragma once

#include <clean-core/algorithm/impl/sort_impl.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

namespace cc
{
/// A "virtual" range the sorting algorithms read from and permute, addressed by index.
///
/// Sorting here never parks an element in a temporary: it only ever reads one and swaps two.
/// That is what lets one call keep several parallel ranges in step, which no move-based sort can do.
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

/// Adapter that sorts one indexed range by a key derived from each element.
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

// =========================================================================================================
// The generic entry points
// =========================================================================================================

/// Sorts the subrange [start, start + size) of a virtual range, ascending by `compare`.
///
/// `compare(a, b)` must be a strict weak ordering and is true when a belongs before b.
/// A comparator that is not one is a contract violation the partitions assert on, rather than memory corruption.
///
/// `should_sort(start, size)` prunes: a subrange it rejects is left unsorted, which is what makes this a selection as
/// well as a sort.
/// Pass cc::constant_function<true>{} to sort everything.
///
/// Deterministic but not stable, and O(n log n) worst case.
/// The range is copied down the recursion, so it must stay cheap — hold pointers in an adapter, not values.
template <class RangeT, class CompareF, class ShouldSortF>
constexpr void sort_ex(isize start, isize size, RangeT range, CompareF&& compare, ShouldSortF&& should_sort)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::sort_ex takes an index_swap_range — see cc::as_index_swap_range");
    static_assert(std::is_trivially_copyable_v<RangeT>, "an index_swap_range must be trivially copyable — hold "
                                                        "pointers, not values");
    CC_ASSERT(size >= 0, "size must be >= 0");

    impl::sort_loop<impl::sort_fallback::heap>(start, size, range, compare, should_sort,
                                               impl::sort_bad_partition_budget(size), true);
}

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

// =========================================================================================================
// Sorting
// =========================================================================================================

/// Sorts an indexed range ascending.
/// Deterministic but not stable; O(n log n) worst case, near-linear on nearly-sorted input.
///
///   cc::sort(values);
///   cc::sort(values, cc::default_greater{});          // descending
///   cc::sort(values, [](auto const& a, auto const& b) { return a.score < b.score; });
template <class RangeT, class CompareF = cc::default_less>
constexpr void sort(RangeT&& values, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort takes an indexed range — one with size() and operator[]");
    cc::sort_ex(0, isize(values.size()), cc::as_index_swap_range(values), compare, cc::constant_function<true>{});
}

/// Sorts an indexed range ascending by a key derived from each element.
/// The key is evaluated on every comparison, so O(n log n) times — cc::sort_by_cached_key evaluates it n times.
///
///   cc::sort_by(entries, &entry::timestamp);          // a pointer-to-member works as a key
///   cc::sort_by(entries, [](auto const& e) { return e.name.size(); });
template <class RangeT, class KeyF, class CompareF = cc::default_less>
constexpr void sort_by(RangeT&& values, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_by takes an indexed range — one with size() and operator[]");
    cc::sort_ex(0, isize(values.size()), cc::as_index_swap_range_by(values, key), compare, cc::constant_function<true>{});
}

template <class RangeT>
constexpr void sort_descending(RangeT&& values)
{
    cc::sort(values, cc::default_greater{});
}

template <class RangeT, class KeyF>
constexpr void sort_by_descending(RangeT&& values, KeyF&& key)
{
    cc::sort_by(values, key, cc::default_greater{});
}

/// Sorts an index range so that the keys it points at come out ordered, leaving the keys untouched.
///
/// Ties break on the index value, so equal keys keep their index order — pass 0..n-1 and the result is stable.
/// Often faster than moving wide elements around, and the way to get several orderings over one data set.
///
///   auto order = cc::vector<i32>::create_defaulted(n);   // fill with 0..n-1
///   cc::sort_indices(order, scores);                     // order[0] indexes the smallest score
template <class IndexRangeT, class KeyRangeT, class CompareF = cc::default_less>
constexpr void sort_indices(IndexRangeT&& indices, KeyRangeT&& keys, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<IndexRangeT>, "cc::sort_indices takes an indexed range of indices");
    static_assert(cc::indexed_range<KeyRangeT>, "cc::sort_indices takes an indexed range of keys");

    auto index_compare = impl::sort_index_compare<std::remove_reference_t<KeyRangeT>, std::remove_reference_t<CompareF>>{
        .keys = &keys,
        .compare = &compare};
    cc::sort_ex(0, isize(indices.size()), cc::as_index_swap_range(indices), index_compare, cc::constant_function<true>{});
}

/// Sorts `keys` and applies the very same permutation to every other range, keeping them in step.
///
/// The comparator comes first because the ranges are variadic; cc::sort_multi_ascending drops it.
/// All ranges must have the same size.
///
///   cc::sort_multi(cc::default_less{}, distances, hit_ids, normals);
template <class CompareF, class KeyRangeT, class... RangeTs>
constexpr void sort_multi(CompareF&& compare, KeyRangeT&& keys, RangeTs&&... values)
{
    static_assert(cc::indexed_range<KeyRangeT> && (cc::indexed_range<RangeTs> && ...), "cc::sort_multi takes indexed "
                                                                                       "ranges");

    isize const size = isize(keys.size());
    CC_ASSERT(((isize(values.size()) == size) && ...), "all ranges must have the same size");

    cc::sort_ex(0, size, cc::as_index_swap_range_multi(keys, values...), compare, cc::constant_function<true>{});
}

template <class KeyRangeT, class... RangeTs>
constexpr void sort_multi_ascending(KeyRangeT&& keys, RangeTs&&... values)
{
    cc::sort_multi(cc::default_less{}, keys, values...);
}

template <class KeyRangeT, class... RangeTs>
constexpr void sort_multi_descending(KeyRangeT&& keys, RangeTs&&... values)
{
    cc::sort_multi(cc::default_greater{}, keys, values...);
}

/// Sorts several parallel ranges by a key built from all of them at once.
/// The key receives one element of every range, in the order the ranges are passed.
///
///   cc::sort_multi_by([](auto x, auto y) { return x * x + y * y; }, cc::default_less{}, xs, ys);
template <class KeyF, class CompareF, class... RangeTs>
constexpr void sort_multi_by(KeyF&& key, CompareF&& compare, RangeTs&&... values)
{
    static_assert(sizeof...(values) >= 1, "cc::sort_multi_by needs at least one range");
    static_assert((cc::indexed_range<RangeTs> && ...), "cc::sort_multi_by takes indexed ranges");

    isize const size = (isize(values.size()), ...);
    CC_ASSERT(((isize(values.size()) == size) && ...), "all ranges must have the same size");

    cc::sort_ex(0, size, cc::as_index_swap_range_multi_by(key, values...), compare, cc::constant_function<true>{});
}

/// Sorts an indexed range by a key that is computed exactly once per element into a temporary buffer.
/// Allocates; worth it as soon as the key costs more than a member read.
template <class RangeT, class KeyF, class CompareF = cc::default_less>
void sort_by_cached_key(RangeT&& values, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_by_cached_key takes an indexed range");

    isize const size = isize(values.size());
    using key_t = std::remove_cvref_t<decltype(cc::invoke(key, values[isize(0)]))>;

    auto keys = cc::vector<key_t>::create_with_capacity(size);
    for (isize i = 0; i < size; ++i)
        keys.push_back(cc::invoke(key, values[i]));

    cc::sort_multi(compare, keys, values);
}

// =========================================================================================================
// Partitioning and selection
// =========================================================================================================

/// Permutes `values` into a left block where `is_right(element)` is false and a right block where it is true.
/// Returns the first index of the right block, i.e. the size of the left one.
/// Runs in O(n) and is not stable.
template <class RangeT, class IsRightF>
constexpr isize partition_by(RangeT&& values, IsRightF&& is_right)
{
    static_assert(cc::indexed_range<RangeT>, "cc::partition_by takes an indexed range");

    auto const range = cc::as_index_swap_range(values);
    auto predicate = impl::sort_element_predicate<decltype(range), std::remove_reference_t<IsRightF>>{.range = range,
                                                                                                      .pred = &is_right};
    return cc::partition_ex(0, isize(values.size()), predicate, range);
}

/// Puts the element that a full sort would place at `idx` there, touching as little else as possible.
///
/// Everything before `idx` also ends up genuinely belonging before it, and everything after after — though
/// neither side is itself sorted.
/// Expected O(n), and O(n) worst case too: a run that keeps splitting badly switches to a median-of-medians pivot.
/// `idx` must be a valid index.
///
/// A stable and an unstable sort put the same value at `idx` — they differ only in which equal-comparing element
/// lands there, so a "stable" spelling of this would buy nothing.
/// cc::sort_indices tiebreaks on the index if you need to name the winner.
template <class RangeT, class CompareF = cc::default_less>
constexpr void sort_at(RangeT&& values, isize idx, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_at takes an indexed range");

    isize const size = isize(values.size());
    CC_ASSERT(0 <= idx && idx < size, "index must be inside the range");

    auto should_sort = impl::sort_index_in_range{.idx = idx};
    impl::sort_loop<impl::sort_fallback::median_of_medians>(0, size, cc::as_index_swap_range(values), compare,
                                                            should_sort, impl::sort_bad_partition_budget(size), true);
}

template <class RangeT, class KeyF, class CompareF = cc::default_less>
constexpr void sort_at_by(RangeT&& values, isize idx, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_at_by takes an indexed range");

    isize const size = isize(values.size());
    CC_ASSERT(0 <= idx && idx < size, "index must be inside the range");

    auto should_sort = impl::sort_index_in_range{.idx = idx};
    impl::sort_loop<impl::sort_fallback::median_of_medians>(0, size, cc::as_index_swap_range_by(values, key), compare,
                                                            should_sort, impl::sort_bad_partition_budget(size), true);
}

/// Puts the elements a full sort would place in `window` there, and sorts just those.
///
/// Everything before and after that window ends up in the right partition without being sorted itself.
/// Takes O(n + window.size log window.size).
/// The window may run past the end; it is clamped to what exists.
///
///   cc::sort_window(scores, {.offset = 10, .size = 5});   // ranks 10..14, in order
template <class RangeT, class CompareF = cc::default_less>
constexpr void sort_window(RangeT&& values, cc::offset_size window, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_window takes an indexed range");
    CC_ASSERT(window.offset >= 0 && window.size >= 0, "window offset and size must be >= 0");

    auto should_sort = impl::sort_overlaps_range{.idx = window.offset, .count = window.size};
    isize const size = isize(values.size());
    impl::sort_loop<impl::sort_fallback::median_of_medians>(0, size, cc::as_index_swap_range(values), compare,
                                                            should_sort, impl::sort_bad_partition_budget(size), true);
}

template <class RangeT, class KeyF, class CompareF = cc::default_less>
constexpr void sort_window_by(RangeT&& values, cc::offset_size window, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_window_by takes an indexed range");
    CC_ASSERT(window.offset >= 0 && window.size >= 0, "window offset and size must be >= 0");

    auto should_sort = impl::sort_overlaps_range{.idx = window.offset, .count = window.size};
    isize const size = isize(values.size());
    impl::sort_loop<impl::sort_fallback::median_of_medians>(0, size, cc::as_index_swap_range_by(values, key), compare,
                                                            should_sort, impl::sort_bad_partition_budget(size), true);
}

/// Puts the `count` smallest elements first, in order, and leaves the rest merely after them.
/// The top-k spelling of cc::sort_window; `count` may run past the end.
///
///   cc::sort_first(scores, 10);   // the ten best, sorted; the rest untouched beyond belonging after
template <class RangeT, class CompareF = cc::default_less>
constexpr void sort_first(RangeT&& values, isize count, CompareF&& compare = {})
{
    cc::sort_window(values, {.offset = 0, .size = count}, compare);
}

// =========================================================================================================
// Queries
// =========================================================================================================

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
