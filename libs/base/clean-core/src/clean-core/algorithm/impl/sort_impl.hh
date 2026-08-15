#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

#include <type_traits>

// pdqsort, expressed over element_get + element_swap so that no element is ever held outside the range.
// The concept these take, the adapters that model it and the whole public surface are clean-core/algorithm/sort.hh's.
//
// Adapted from pdqsort (https://github.com/orlp/pdqsort), zlib-licensed, (c) 2021 Orson Peters.
// The upstream cyclic-permutation variant of the block partition is absent on purpose: it parks an element in a
// temporary, which is the one thing this formulation may not do.

namespace cc::impl
{
enum : isize
{
    /// Ranges at or below this are insertion-sorted.
    sort_insertion_threshold = 16,

    /// Above this the pivot is a pseudomedian of nine rather than a median of three.
    sort_ninther_threshold = 128,

    /// How many positions partial insertion sort may move elements over before it gives up.
    sort_partial_insertion_limit = 8,

    /// Elements classified per pass of the block partition.
    /// The offset buffers are u8, so this must stay <= 256.
    sort_block_size = 64,
};

/// What a sort does once its bad-partition budget is spent.
enum class sort_fallback
{
    /// Heapsort the rest of the range, which bounds a full sort at O(n log n).
    heap,

    /// Switch to a median-of-medians pivot, which keeps a pruned (selection-style) run linear.
    median_of_medians,
};

/// The value an adapter hands back, with references and cv stripped.
template <class RangeT>
using sort_element_t = std::remove_cvref_t<decltype(static_cast<RangeT*>(nullptr)->element_get(isize(0)))>;

/// Whether the pivot is snapshotted into a local rather than bound as a reference into the range being permuted.
template <class RangeT>
inline constexpr bool sort_pivot_by_value = std::is_trivially_copyable_v<sort_element_t<RangeT>> //
                                         && sizeof(sort_element_t<RangeT>) <= 32;

/// How a partition holds its pivot.
/// A reference here is safe in both directions: a computed element_get yields a temporary whose lifetime the
/// binding extends, and a reference into the range stays valid because index `start` is not swapped until the
/// partition is done.
template <class RangeT>
using sort_pivot_t
    = std::conditional_t<sort_pivot_by_value<RangeT>, sort_element_t<RangeT>, sort_element_t<RangeT> const&>;

/// Whether to classify branchlessly into an offset buffer instead of branching per element.
/// A heuristic on the element, standing in for "the comparison is cheap enough that misprediction dominates".
template <class RangeT>
inline constexpr bool sort_use_block_partition = sort_pivot_by_value<RangeT>;

/// Sort-subrange predicate selecting only the subranges containing `idx`.
struct sort_index_in_range
{
    isize idx;

    constexpr bool operator()(isize start, isize size) const { return start <= idx && idx < start + size; }
};

/// Sort-subrange predicate selecting only the subranges overlapping [idx, idx + count).
struct sort_overlaps_range
{
    isize idx;
    isize count;

    constexpr bool operator()(isize start, isize size) const { return start <= idx + count && idx <= start + size; }
};

/// Orders index values by the keys they point at, breaking ties on the index itself.
/// The tiebreak makes this a strict total order, so equal keys never reach the equal-elements partition.
template <class KeyRangeT, class CompareF>
struct sort_index_compare
{
    KeyRangeT* keys;
    CompareF* compare;

    template <class A, class B>
    constexpr bool operator()(A const& ia, B const& ib) const
    {
        auto const& key_a = (*keys)[isize(ia)];
        auto const& key_b = (*keys)[isize(ib)];

        if ((*compare)(key_a, key_b))
            return true;
        if ((*compare)(key_b, key_a))
            return false;
        return ia < ib;
    }
};

// =========================================================================================================
// Small sorts
// =========================================================================================================

template <class RangeT, class CompareF>
constexpr void sort_order2(isize ia, isize ib, RangeT range, CompareF& compare)
{
    if (compare(range.element_get(ib), range.element_get(ia)))
        range.element_swap(ia, ib);
}

template <class RangeT, class CompareF>
constexpr void sort_order3(isize ia, isize ib, isize ic, RangeT range, CompareF& compare)
{
    impl::sort_order2(ia, ib, range, compare);
    impl::sort_order2(ib, ic, range, compare);
    impl::sort_order2(ia, ib, range, compare);
}

/// Insertion sort over [start, start + size), bounded at `start`.
template <class RangeT, class CompareF>
constexpr void sort_insertion(isize start, isize size, RangeT range, CompareF& compare)
{
    for (isize i = start + 1; i < start + size; ++i)
        for (isize j = i; j > start && compare(range.element_get(j), range.element_get(j - 1)); --j)
            range.element_swap(j - 1, j);
}

/// Insertion sort without the lower bound test.
/// Some element before `start` must compare not-greater than every element of [start, start + size), which is
/// what stops the inner loop.
template <class RangeT, class CompareF>
constexpr void sort_insertion_unguarded(isize start, isize size, RangeT range, CompareF& compare)
{
    for (isize i = start + 1; i < start + size; ++i)
    {
        isize j = i;
        while (compare(range.element_get(j), range.element_get(j - 1)))
        {
            range.element_swap(j - 1, j);
            --j;
            CC_ASSERT(j >= start, "comparison function is not a strict weak ordering");
        }
    }
}

/// Insertion sort that gives up once it has moved elements too far.
/// Returns false when it gave up, leaving the range partially sorted.
template <class RangeT, class CompareF>
constexpr bool sort_partial_insertion(isize start, isize size, RangeT range, CompareF& compare)
{
    if (size < 2)
        return true;

    isize moves = 0;
    for (isize i = start + 1; i < start + size; ++i)
    {
        isize j = i;
        while (j > start && compare(range.element_get(j), range.element_get(j - 1)))
        {
            range.element_swap(j - 1, j);
            --j;
        }

        moves += i - j;
        if (moves > sort_partial_insertion_limit)
            return false;
    }

    return true;
}

// =========================================================================================================
// Heapsort — the worst-case fallback, and swap-only like everything else here
// =========================================================================================================

template <class RangeT, class CompareF>
constexpr void sort_heap_sift_down(isize start, isize size, isize root, RangeT range, CompareF& compare)
{
    while (true)
    {
        isize child = 2 * root + 1;
        if (child >= size)
            return;

        if (child + 1 < size && compare(range.element_get(start + child), range.element_get(start + child + 1)))
            ++child;

        if (!compare(range.element_get(start + root), range.element_get(start + child)))
            return;

        range.element_swap(start + root, start + child);
        root = child;
    }
}

template <class RangeT, class CompareF>
constexpr void sort_heapsort(isize start, isize size, RangeT range, CompareF& compare)
{
    for (isize i = size / 2 - 1; i >= 0; --i)
        impl::sort_heap_sift_down(start, size, i, range, compare);

    for (isize i = size - 1; i > 0; --i)
    {
        range.element_swap(start, start + i);
        impl::sort_heap_sift_down(start, i, 0, range, compare);
    }
}

// =========================================================================================================
// Partitioning
// =========================================================================================================

/// Partitions [start, start + size) around the pivot sitting at `start`, elements equal to it going right.
/// Returns the pivot's final index; out_already_partitioned reports that nothing had to move.
/// The pivot must be the median of at least three of the range's elements — that is what bounds both scans.
template <class RangeT, class CompareF>
constexpr isize sort_partition_right(isize start, isize size, RangeT range, CompareF& compare, bool& out_already_partitioned)
{
    isize const end = start + size;
    sort_pivot_t<RangeT> pivot = range.element_get(start);

    isize first = start;
    isize last = end;

    // the first element not less than the pivot, which the median-of-three guarantees exists
    while (true)
    {
        ++first;
        CC_ASSERT(first < end, "comparison function is not a strict weak ordering");
        if (!compare(range.element_get(first), pivot))
            break;
    }

    // the last element strictly less than the pivot, needing a bound only when the scan above skipped nothing
    if (first - 1 == start)
    {
        while (first < last)
        {
            --last;
            if (compare(range.element_get(last), pivot))
                break;
        }
    }
    else
    {
        while (true)
        {
            --last;
            CC_ASSERT(last > start, "comparison function is not a strict weak ordering");
            if (compare(range.element_get(last), pivot))
                break;
        }
    }

    out_already_partitioned = first >= last;

    while (first < last)
    {
        range.element_swap(first, last);

        while (true)
        {
            ++first;
            CC_ASSERT(first < end, "comparison function is not a strict weak ordering");
            if (!compare(range.element_get(first), pivot))
                break;
        }
        while (true)
        {
            --last;
            CC_ASSERT(last > start, "comparison function is not a strict weak ordering");
            if (compare(range.element_get(last), pivot))
                break;
        }
    }

    isize const pivot_pos = first - 1;
    if (pivot_pos != start)
        range.element_swap(start, pivot_pos);
    return pivot_pos;
}

/// sort_partition_right with the element classification made branchless.
/// Each pass records the offsets of the misplaced elements of a block and then swaps them pairwise, so a
/// mispredictable branch per element becomes a predictable one per block.
/// Only offsets are buffered — every element stays inside the range throughout.
template <class RangeT, class CompareF>
constexpr isize sort_partition_right_blocks(isize start,
                                            isize size,
                                            RangeT range,
                                            CompareF& compare,
                                            bool& out_already_partitioned)
{
    isize const end = start + size;
    sort_pivot_t<RangeT> pivot = range.element_get(start);

    isize first = start;
    isize last = end;

    while (true)
    {
        ++first;
        CC_ASSERT(first < end, "comparison function is not a strict weak ordering");
        if (!compare(range.element_get(first), pivot))
            break;
    }

    if (first - 1 == start)
    {
        while (first < last)
        {
            --last;
            if (compare(range.element_get(last), pivot))
                break;
        }
    }
    else
    {
        while (true)
        {
            --last;
            CC_ASSERT(last > start, "comparison function is not a strict weak ordering");
            if (compare(range.element_get(last), pivot))
                break;
        }
    }

    out_already_partitioned = first >= last;

    if (!out_already_partitioned)
    {
        range.element_swap(first, last);
        ++first;

        // offsets_l holds distances forward from offsets_l_base, offsets_r distances backward from offsets_r_base
        u8 offsets_l[sort_block_size] = {};
        u8 offsets_r[sort_block_size] = {};
        isize offsets_l_base = first;
        isize offsets_r_base = last;
        isize num_l = 0;
        isize num_r = 0;
        isize start_l = 0;
        isize start_r = 0;

        while (first < last)
        {
            // a side that still has unswapped offsets recorded is not refilled
            isize const num_unknown = last - first;
            isize const left_split = num_l == 0 ? (num_r == 0 ? num_unknown / 2 : num_unknown) : 0;
            isize const right_split = num_r == 0 ? num_unknown - left_split : 0;

            isize const left_count = cc::min(left_split, isize(sort_block_size));
            for (isize i = 0; i < left_count; ++i)
            {
                offsets_l[num_l] = u8(i);
                num_l += isize(!compare(range.element_get(first), pivot));
                ++first;
            }

            isize const right_count = cc::min(right_split, isize(sort_block_size));
            for (isize i = 0; i < right_count; ++i)
            {
                --last;
                offsets_r[num_r] = u8(i + 1);
                num_r += isize(compare(range.element_get(last), pivot));
            }

            isize const num = cc::min(num_l, num_r);
            for (isize i = 0; i < num; ++i)
                range.element_swap(offsets_l_base + offsets_l[start_l + i], offsets_r_base - offsets_r[start_r + i]);

            num_l -= num;
            num_r -= num;
            start_l += num;
            start_r += num;

            if (num_l == 0)
            {
                start_l = 0;
                offsets_l_base = first;
            }
            if (num_r == 0)
            {
                start_r = 0;
                offsets_r_base = last;
            }
        }

        // one side can be left holding offsets the other had no partner for
        if (num_l > 0)
        {
            while (num_l > 0)
            {
                --num_l;
                --last;
                range.element_swap(offsets_l_base + offsets_l[start_l + num_l], last);
            }
            first = last;
        }
        if (num_r > 0)
        {
            while (num_r > 0)
            {
                --num_r;
                range.element_swap(offsets_r_base - offsets_r[start_r + num_r], first);
                ++first;
            }
            last = first;
        }
    }

    isize const pivot_pos = first - 1;
    if (pivot_pos != start)
        range.element_swap(start, pivot_pos);
    return pivot_pos;
}

/// Partitions [start, start + size) around the pivot at `start`, elements equal to it going LEFT.
/// The pivot must be the smallest value in the range, which both bounds the scans and makes this the step that
/// keeps duplicate-heavy input out of quadratic behaviour.
/// Returns the pivot's final index.
template <class RangeT, class CompareF>
constexpr isize sort_partition_left(isize start, isize size, RangeT range, CompareF& compare)
{
    isize const end = start + size;
    sort_pivot_t<RangeT> pivot = range.element_get(start);

    isize first = start;
    isize last = end;

    while (true)
    {
        --last;
        CC_ASSERT(last >= start, "comparison function is not a strict weak ordering");
        if (!compare(pivot, range.element_get(last)))
            break;
    }

    if (last + 1 == end)
    {
        while (first < last)
        {
            ++first;
            if (compare(pivot, range.element_get(first)))
                break;
        }
    }
    else
    {
        while (true)
        {
            ++first;
            CC_ASSERT(first < end, "comparison function is not a strict weak ordering");
            if (compare(pivot, range.element_get(first)))
                break;
        }
    }

    while (first < last)
    {
        range.element_swap(first, last);

        while (true)
        {
            --last;
            CC_ASSERT(last >= start, "comparison function is not a strict weak ordering");
            if (!compare(pivot, range.element_get(last)))
                break;
        }
        while (true)
        {
            ++first;
            CC_ASSERT(first < end, "comparison function is not a strict weak ordering");
            if (compare(pivot, range.element_get(first)))
                break;
        }
    }

    isize const pivot_pos = last;
    if (pivot_pos != start)
        range.element_swap(start, pivot_pos);
    return pivot_pos;
}

/// Deterministic partial shuffle of both partitions after a badly unbalanced split.
/// A pattern that produced one bad split then cannot go on producing them.
template <class RangeT>
constexpr void sort_break_patterns(isize start, isize left_size, isize pivot_pos, isize right_size, RangeT range)
{
    isize const end = pivot_pos + 1 + right_size;

    if (left_size >= sort_insertion_threshold)
    {
        isize const quarter = left_size / 4;
        range.element_swap(start, start + quarter);
        range.element_swap(pivot_pos - 1, pivot_pos - quarter);

        if (left_size > sort_ninther_threshold)
        {
            range.element_swap(start + 1, start + quarter + 1);
            range.element_swap(start + 2, start + quarter + 2);
            range.element_swap(pivot_pos - 2, pivot_pos - quarter - 1);
            range.element_swap(pivot_pos - 3, pivot_pos - quarter - 2);
        }
    }

    if (right_size >= sort_insertion_threshold)
    {
        isize const quarter = right_size / 4;
        range.element_swap(pivot_pos + 1, pivot_pos + quarter + 1);
        range.element_swap(end - 1, end - quarter);

        if (right_size > sort_ninther_threshold)
        {
            range.element_swap(pivot_pos + 2, pivot_pos + quarter + 2);
            range.element_swap(pivot_pos + 3, pivot_pos + quarter + 3);
            range.element_swap(end - 2, end - quarter - 1);
            range.element_swap(end - 3, end - quarter - 2);
        }
    }
}

// =========================================================================================================
// The sort itself
// =========================================================================================================

template <sort_fallback Fallback, class RangeT, class CompareF, class ShouldSortF>
constexpr void sort_loop(isize start,
                         isize size,
                         RangeT range,
                         CompareF& compare,
                         ShouldSortF& should_sort,
                         isize bad_allowed,
                         bool leftmost);

/// Moves the median of medians of [start, start + size) to `start`.
/// Costs O(size) but splits the range at worst 30/70, which is what bounds a pruned run linearly.
template <class RangeT, class CompareF>
constexpr void sort_pivot_median_of_medians(isize start, isize size, RangeT range, CompareF& compare)
{
    // each group of five is sorted in place and its median moved into [start, start + groups)
    isize const groups = (size + 4) / 5;
    for (isize g = 0; g < groups; ++g)
    {
        isize const group_start = start + g * 5;
        isize const group_size = cc::min(isize(5), start + size - group_start);
        impl::sort_insertion(group_start, group_size, range, compare);

        isize const median = group_start + group_size / 2;
        if (start + g != median)
            range.element_swap(start + g, median);
    }

    if (groups > 1)
    {
        auto should_sort_median = impl::sort_index_in_range{.idx = start + groups / 2};
        impl::sort_loop<sort_fallback::median_of_medians>(start, groups, range, compare, should_sort_median, isize(0),
                                                          true);
    }

    if (groups / 2 != 0)
        range.element_swap(start, start + groups / 2);
}

/// What one pdqsort iteration left behind: the two subranges that still want sorting.
///
/// All-zero means nothing is left — small-sorted, heapsorted, finished off by partial insertion, or pruned away
/// by `should_sort`.
/// `left` is recursed into and `tail` looped on, which is what the sequential driver does; a parallel one hands
/// them to two tasks, which is sound because everything reaching across the pivot has already happened.
struct sort_step_result
{
    isize left_start = 0;
    isize left_size = 0;
    isize tail_start = 0;
    isize tail_size = 0;
    isize bad_allowed = 0;
    bool tail_leftmost = false;
};

/// One iteration of the pdqsort driver: pick a pivot, partition, report what is left.
///
/// Extracted so the sequential and the parallel driver share it rather than drifting apart — sort_loop is a
/// loop around this, and cc::sort_async spawns around it.
/// Everything touching BOTH partitions finishes before this returns: break_patterns reaches across the pivot,
/// and so does the partial-insertion probe.
/// That is precisely what makes the two reported subranges safe to hand to two threads.
///
/// CC_FORCE_INLINE is load-bearing rather than a hint.
/// Left to itself the compiler emits this out of line, and sort_loop then pays a real call per iteration where
/// it used to be one fused loop — which measured.
template <sort_fallback Fallback, class RangeT, class CompareF, class ShouldSortF>
CC_FORCE_INLINE constexpr sort_step_result sort_step(isize start,
                                                     isize size,
                                                     RangeT range,
                                                     CompareF& compare,
                                                     ShouldSortF& should_sort,
                                                     isize bad_allowed,
                                                     bool leftmost)
{
    if (size <= sort_insertion_threshold)
    {
        if (leftmost)
            impl::sort_insertion(start, size, range, compare);
        else
            impl::sort_insertion_unguarded(start, size, range, compare);
        return {};
    }

    isize const end = start + size;
    isize const half = size / 2;

    bool use_median_of_medians = false;
    if constexpr (Fallback == sort_fallback::median_of_medians)
        use_median_of_medians = bad_allowed <= 0;

    if (use_median_of_medians)
    {
        impl::sort_pivot_median_of_medians(start, size, range, compare);
    }
    else if (size > sort_ninther_threshold)
    {
        impl::sort_order3(start, start + half, end - 1, range, compare);
        impl::sort_order3(start + 1, start + half - 1, end - 2, range, compare);
        impl::sort_order3(start + 2, start + half + 1, end - 3, range, compare);
        impl::sort_order3(start + half - 1, start + half, start + half + 1, range, compare);
        range.element_swap(start, start + half);
    }
    else
    {
        impl::sort_order3(start + half, start, end - 1, range, compare);
    }

    // the element just before the range equals the pivot, so every element equal to it belongs left of here
    if (!leftmost && !compare(range.element_get(start - 1), range.element_get(start)))
    {
        isize const pivot_pos = impl::sort_partition_left(start, size, range, compare);

        sort_step_result r;
        r.tail_start = pivot_pos + 1;
        r.tail_size = end - r.tail_start;
        r.bad_allowed = bad_allowed;
        r.tail_leftmost = false; // it was already false to reach this branch
        if (!should_sort(r.tail_start, r.tail_size))
            r.tail_size = 0;
        return r;
    }

    bool already_partitioned = false;
    isize pivot_pos = 0;
    if constexpr (sort_use_block_partition<RangeT>)
        pivot_pos = impl::sort_partition_right_blocks(start, size, range, compare, already_partitioned);
    else
        pivot_pos = impl::sort_partition_right(start, size, range, compare, already_partitioned);

    isize const left_size = pivot_pos - start;
    isize const right_size = end - (pivot_pos + 1);

    if (left_size < size / 8 || right_size < size / 8)
    {
        --bad_allowed;
        if constexpr (Fallback == sort_fallback::heap)
        {
            if (bad_allowed <= 0)
            {
                impl::sort_heapsort(start, size, range, compare);
                return {};
            }
        }

        impl::sort_break_patterns(start, left_size, pivot_pos, right_size, range);
    }
    else if (already_partitioned                                               //
             && impl::sort_partial_insertion(start, left_size, range, compare) //
             && impl::sort_partial_insertion(pivot_pos + 1, right_size, range, compare))
    {
        return {}; // both halves were nearly sorted and now fully are, whatever `should_sort` asked for
    }

    sort_step_result r;
    r.left_start = start;
    r.left_size = should_sort(start, left_size) ? left_size : 0;
    r.tail_start = pivot_pos + 1;
    r.tail_size = should_sort(r.tail_start, right_size) ? right_size : 0;
    r.bad_allowed = bad_allowed;
    r.tail_leftmost = false;
    return r;
}

/// The pdqsort driver: pick a pivot, partition, recurse into the left half and loop on the right.
/// `should_sort` prunes whole subranges, which is what turns the same machinery into a selection.
/// `leftmost` states that no element before `start` is known to bound the range from below.
template <sort_fallback Fallback, class RangeT, class CompareF, class ShouldSortF>
constexpr void sort_loop(isize start,
                         isize size,
                         RangeT range,
                         CompareF& compare,
                         ShouldSortF& should_sort,
                         isize bad_allowed,
                         bool leftmost)
{
    while (true)
    {
        // the small-sort leaf is handled here rather than through sort_step, which would cost it a whole
        // sort_step_result round trip — it is the most frequently reached path in the whole sort
        if (size <= sort_insertion_threshold)
        {
            if (leftmost)
                impl::sort_insertion(start, size, range, compare);
            else
                impl::sort_insertion_unguarded(start, size, range, compare);
            return;
        }

        auto const step = impl::sort_step<Fallback>(start, size, range, compare, should_sort, bad_allowed, leftmost);

        // the left recursion takes the CURRENT leftmost, not the tail's
        if (step.left_size > 0)
            impl::sort_loop<Fallback>(step.left_start, step.left_size, range, compare, should_sort, step.bad_allowed,
                                      leftmost);

        if (step.tail_size <= 0)
            return;

        start = step.tail_start;
        size = step.tail_size;
        bad_allowed = step.bad_allowed;
        leftmost = step.tail_leftmost;
    }
}

/// The bad-partition budget a range of this size starts with, i.e. roughly log2(size).
constexpr isize sort_bad_partition_budget(isize size)
{
    return isize(cc::bit_width(u64(size < 1 ? 1 : size)));
}
} // namespace cc::impl
