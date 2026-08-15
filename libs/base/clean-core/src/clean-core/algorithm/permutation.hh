#pragma once

#include <clean-core/algorithm/index_swap_range.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/range_traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

// Rearranging a range by position rather than by comparison.
//
// All of it is swap-only, so it composes with everything else in this topic and honours the same no-parking rule:
// cycle-following moves an element straight to where it belongs, never through a temporary.

namespace cc
{
// =========================================================================================================
// Reverse and rotate
// =========================================================================================================

/// Reverses [start, start + size) in place.
template <class RangeT>
constexpr void reverse_ex(isize start, isize size, RangeT range)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::reverse_ex takes an index_swap_range — see "
                                                "cc::as_index_swap_range");
    CC_ASSERT(size >= 0, "size must be >= 0");

    for (isize lo = start, hi = start + size - 1; lo < hi; ++lo, --hi)
        range.element_swap(lo, hi);
}

/// Rotates [start, start + size) left by `count`, so the element at start + count ends up at start.
///
/// `count` is normalized into [0, size), so a full turn or a negative count is well defined rather than a
/// precondition — rotating left by -1 is rotating right by 1.
/// Three reversals, hence O(size) swaps and no temporary.
template <class RangeT>
constexpr void rotate_ex(isize start, isize size, isize count, RangeT range)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::rotate_ex takes an index_swap_range — see "
                                                "cc::as_index_swap_range");
    CC_ASSERT(size >= 0, "size must be >= 0");

    if (size <= 1)
        return;

    count %= size;
    if (count < 0)
        count += size;
    if (count == 0)
        return;

    cc::reverse_ex(start, count, range);
    cc::reverse_ex(start + count, size - count, range);
    cc::reverse_ex(start, size, range);
}

/// Reverses `values` in place.
template <class RangeT>
constexpr void reverse(RangeT&& values)
{
    static_assert(cc::indexed_range<RangeT>, "cc::reverse takes an indexed range");
    cc::reverse_ex(0, isize(values.size()), cc::as_index_swap_range(values));
}

/// Rotates `values` left by `count`, so the element at `count` ends up first.
/// `count` is normalized into [0, size), so any value is legal.
///
///   cc::rotate(values, 1);    // first element moves to the back
///   cc::rotate(values, -1);   // last element moves to the front
template <class RangeT>
constexpr void rotate(RangeT&& values, isize count)
{
    static_assert(cc::indexed_range<RangeT>, "cc::rotate takes an indexed range");
    cc::rotate_ex(0, isize(values.size()), count, cc::as_index_swap_range(values));
}

// =========================================================================================================
// Permutations
// =========================================================================================================

/// Rearranges [0, size) so that position i ends up holding what index `indices[i]` pointed at.
///
/// CONSUMES `indices`: it comes out as the identity 0..size-1.
/// That is what buys guaranteed O(size) — the non-destructive spelling has to re-walk each cycle and goes
/// quadratic on the worst permutation.
/// Pass a scratch array, which is what cc::sort_indices produces anyway.
///
/// `indices` must be a permutation of 0..size-1; anything else is a contract violation the bounds assert only
/// partly catches.
/// Nothing is parked: each swap puts at least one element in its final place.
template <class IndexRangeT, class RangeT>
constexpr void apply_permutation_ex(isize size, IndexRangeT& indices, RangeT range)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::apply_permutation_ex takes an index_swap_range — see "
                                                "cc::as_index_swap_range");
    CC_ASSERT(size >= 0, "size must be >= 0");
    CC_ASSERT(isize(indices.size()) >= size, "indices must cover the whole range");

    for (isize i = 0; i < size; ++i)
    {
        while (isize(indices[i]) != i)
        {
            isize const source = isize(indices[i]);
            CC_ASSERT(0 <= source && source < size, "indices must be a permutation of 0..size-1");

            isize const target = isize(indices[source]);
            CC_ASSERT(0 <= target && target < size, "indices must be a permutation of 0..size-1");

            // source is never its own image here: two entries holding `source` would not be a permutation
            range.element_swap(source, target);
            cc::swap(indices[i], indices[source]);
        }
    }
}

/// Rearranges `values` so that values[i] ends up holding what indices[i] pointed at.
///
/// CONSUMES `indices`, which comes out as the identity — see apply_permutation_ex for why.
/// Together with cc::sort_indices this is a stable sort:
///
///   auto order = cc::vector<i32>::create_defaulted(n);   // fill with 0..n-1
///   cc::sort_indices(order, values);                     // ties break on the index, so this is stable
///   cc::apply_permutation(values, order);                // now values is stably sorted
///
/// cc::sort_stable does exactly that in one call.
template <class RangeT, class IndexRangeT>
constexpr void apply_permutation(RangeT&& values, IndexRangeT&& indices)
{
    static_assert(cc::indexed_range<RangeT>, "cc::apply_permutation takes an indexed range");
    static_assert(cc::indexed_range<IndexRangeT>, "cc::apply_permutation takes an indexed range of indices");
    CC_ASSERT(isize(indices.size()) == isize(values.size()), "indices and values must have the same size");

    cc::apply_permutation_ex(isize(values.size()), indices, cc::as_index_swap_range(values));
}

/// Replaces `indices` with its inverse: where it said "position i draws from j", it comes out saying
/// "what was at j belongs at i".
///
/// Inverting cc::sort_indices' output turns a gather order into RANKS — indices[i] becomes the position element
/// i sorts to.
/// Runs in O(n) by walking each cycle once and marking visited entries as ~x, so the index type must be SIGNED
/// and wide enough to hold ~(n-1), which every signed type is.
template <class IndexRangeT>
constexpr void invert_permutation(IndexRangeT&& indices)
{
    static_assert(cc::indexed_range<IndexRangeT>, "cc::invert_permutation takes an indexed range of indices");

    using index_t = std::remove_cvref_t<decltype(indices[isize(0)])>;
    static_assert(std::is_signed_v<index_t>, "cc::invert_permutation marks visited entries as ~x, so the index "
                                             "type must be signed");

    isize const size = isize(indices.size());

    for (isize i = 0; i < size; ++i)
    {
        if (isize(indices[i]) < 0)
            continue; // this cycle was already inverted

        isize previous = i;
        isize current = isize(indices[i]);
        CC_ASSERT(0 <= current && current < size, "indices must be a permutation of 0..size-1");

        while (current != i)
        {
            isize const next = isize(indices[current]);
            CC_ASSERT(0 <= next && next < size, "indices must be a permutation of 0..size-1");

            indices[current] = index_t(~previous);
            previous = current;
            current = next;
        }
        indices[i] = index_t(~previous);
    }

    for (isize i = 0; i < size; ++i)
        indices[i] = index_t(~isize(indices[i]));
}
} // namespace cc
