#pragma once

#include <clean-core/algorithm/impl/sort_impl.hh>
#include <clean-core/algorithm/index_swap_range.hh>
#include <clean-core/algorithm/permutation.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/common/range_traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

// The sorting family, over the index_swap_range seam.
// The seam itself, the adapters over it, partitioning and the orderedness queries are
// clean-core/algorithm/index_swap_range.hh's — none of them need the pdqsort machinery this header pulls in.

namespace cc
{
// =========================================================================================================
// The generic entry point
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

    isize const size = [](auto& first, auto&...) { return isize(first.size()); }(values...);
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

/// Sorts an indexed range ascending, keeping equal elements in their original relative order.
///
/// Allocates an INDEX array, never an element buffer — so the no-parking rule survives:
/// cc::sort_indices already breaks ties on the index, and cc::apply_permutation is swap-only.
/// Takes O(n log n) and one allocation of n indices.
///
/// This is why there is no buffered merge sort here: for anything wider than an index the permutation route
/// moves less memory, and an in-place block merge sort (WikiSort / GrailSort) buys O(1) space at 2-3x the time
/// and a large implementation.
template <class RangeT, class CompareF = cc::default_less>
void sort_stable(RangeT&& values, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_stable takes an indexed range");

    isize const size = isize(values.size());
    auto order = cc::vector<isize>::create_defaulted(size);
    for (isize i = 0; i < size; ++i)
        order[i] = i;

    cc::sort_indices(order, values, compare);
    cc::apply_permutation(values, order);
}

/// Sorts an indexed range ascending by a key, keeping elements with equal keys in their original relative order.
/// The key is evaluated on every comparison, as in cc::sort_by.
template <class RangeT, class KeyF, class CompareF = cc::default_less>
void sort_stable_by(RangeT&& values, KeyF&& key, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_stable_by takes an indexed range");

    cc::sort_stable(
        values, [&](auto const& a, auto const& b) { return bool(compare(cc::invoke(key, a), cc::invoke(key, b))); });
}

/// Sorts `values`, drops the duplicates and shrinks it to what is left, returning the new size.
///
/// The sort + unique + erase idiom as one call, and the reason cc::is_strictly_sorted exists to check it.
/// Not stable, and it says nothing about WHICH of several equivalent elements survives — cc::sort_stable first,
/// then cc::dedup_sorted_ex, if that matters.
///
///   auto const n = cc::sort_and_dedup(ids);   // ids is now sorted, unique and n long
///
/// Takes a container: it must be able to shrink itself.
/// For a view, sort it and call cc::dedup_sorted_ex.
template <class RangeT, class CompareF = cc::default_less>
isize sort_and_dedup(RangeT&& values, CompareF&& compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_and_dedup takes an indexed range");
    static_assert(
        requires(RangeT& r) { r.resize_down_to(isize(0)); },
        "cc::sort_and_dedup shrinks its argument, so it takes a container rather than a view — sort a "
        "view and call cc::dedup_sorted_ex instead");

    cc::sort(values, compare);

    isize const new_size = cc::dedup_sorted_ex(0, isize(values.size()), cc::as_index_swap_range(values), compare);
    values.resize_down_to(new_size);
    return new_size;
}

// =========================================================================================================
// Selection
// =========================================================================================================

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

} // namespace cc
