#pragma once

#include <clean-core/algorithm/impl/sort_impl.hh>
#include <clean-core/algorithm/index_swap_range.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/common/range_traits.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/async.hh>

#include <type_traits>

// cc::sort — the same pdqsort, fanned out across a scheduler.
//
// Its own header because thread/async.hh is heavy and clean-core/algorithm/sort.hh must not pull it in.
// The dependency runs one way: this includes sort.hh's machinery, never the reverse.
//
// Parallel recursion over a SEQUENTIAL partition, which is what caps the speedup.
// The first partition is a serial pass over the whole range, and the top few are DRAM-bandwidth-bound rather than
// compute-bound, so they neither speed up nor overlap with anything.
// A parallel partition (ips4o-style block permutation, which would also stay swap-only) is the future slice.

namespace cc
{
/// Below this a subproblem is finished by the ordinary synchronous sort instead of being spawned.
///
/// Swept, not guessed — see docs/benchmarks/sort-benchmark.md.
/// Speedup rises monotonically as the cutoff falls (1024 measured best at 7.2x, 16384 at 6.7x), so this sits one
/// step off the measured optimum: 4096 keeps a quarter of 1024's task count for about 3% less speedup.
inline constexpr isize sort_async_default_cutoff = 4 * 1024;
} // namespace cc

namespace cc::impl
{
template <class RangeT, class CompareF>
struct sort_async_frame;

template <class RangeT, class CompareF>
[[nodiscard]] cc::shared_async<cc::unit> make_sort_async_task(RangeT range,
                                                              CompareF const& compare,
                                                              isize start,
                                                              isize size,
                                                              isize bad_allowed,
                                                              bool leftmost,
                                                              isize cutoff);

/// One task: run a pdqsort step, hand each surviving half that is still big to a task of its own, and park.
///
/// The frame is around 56 B, so it does NOT fit the async node's 32 B inline slot and is heap-boxed — one extra
/// allocation per task, deliberately accepted.
/// At the default cutoff a 10M-element sort spawns ~1200 tasks, i.e. ~50 us of boxing against ~200 ms of sorting.
/// Do NOT add the static_assert(sizeof(F) <= 32) the async benchmarks carry: their leaves are ~100 ns, where a
/// malloc really is 40% of the task, and ours are ~300 us.
template <class RangeT, class CompareF>
struct sort_async_frame
{
    RangeT range;
    CompareF compare;
    isize start = 0;
    isize size = 0;
    isize cutoff = 0;
    cc::shared_async<cc::unit> left;
    cc::shared_async<cc::unit> tail;
    i32 bad_allowed = 0;
    bool leftmost = true;

    /// Whether the split already happened, kept explicitly rather than inferred from `left == nullptr`.
    /// A null `left` is legitimate here — it means the left half fitted under the cutoff — so the null test
    /// the fork-join benchmarks use would re-enter the split on the join poll and partition the range twice.
    bool spawned = false;

    cc::async_step_status operator()(cc::async_context<cc::unit>& actx)
    {
        if (spawned)
        {
            // a raw frame gets no automatic propagation, so the join has to look
            if (left != nullptr && left->has_error())
                return actx.resolve_to_error(left->propagate_error());
            if (tail != nullptr && tail->has_error())
                return actx.resolve_to_error(tail->propagate_error());
            return actx.success(cc::unit{});
        }

        auto should_sort = cc::constant_function<true>{};

        if (size <= cutoff)
        {
            impl::sort_loop<impl::sort_fallback::heap>(start, size, range, compare, should_sort, isize(bad_allowed),
                                                       leftmost);
            return actx.success(cc::unit{});
        }

        auto const step = impl::sort_step<impl::sort_fallback::heap>(start, size, range, compare, should_sort,
                                                                     isize(bad_allowed), leftmost);

        // `step` is complete: break_patterns and the partial-insertion probe both reach across the pivot and
        // both have already run, so from here the two subranges are disjoint and private to their task
        if (step.left_size > cutoff)
            left = impl::make_sort_async_task(range, compare, step.left_start, step.left_size, step.bad_allowed,
                                              leftmost, cutoff);
        else if (step.left_size > 0)
            impl::sort_loop<impl::sort_fallback::heap>(step.left_start, step.left_size, range, compare, should_sort,
                                                       step.bad_allowed, leftmost);

        if (step.tail_size > cutoff)
            tail = impl::make_sort_async_task(range, compare, step.tail_start, step.tail_size, step.bad_allowed,
                                              step.tail_leftmost, cutoff);
        else if (step.tail_size > 0)
            impl::sort_loop<impl::sort_fallback::heap>(step.tail_start, step.tail_size, range, compare, should_sort,
                                                       step.bad_allowed, step.tail_leftmost);

        if (left == nullptr && tail == nullptr)
            return actx.success(cc::unit{}); // both halves fitted under the cutoff and are done

        // require EVERY child before testing readiness, so both register as pending
        bool all_ready = true;
        if (left != nullptr)
            all_ready = actx.require(left) && all_ready;
        if (tail != nullptr)
            all_ready = actx.require(tail) && all_ready;

        // a freshly created lazy child cannot already be ready, but parking with an EMPTY pending list would
        // hang forever, and two lines are cheaper than that failure mode
        if (all_ready)
            return actx.success(cc::unit{});

        spawned = true;
        return actx.wait_for_dependencies();
    }
};

template <class RangeT, class CompareF>
[[nodiscard]] cc::shared_async<cc::unit> make_sort_async_task(RangeT range,
                                                              CompareF const& compare,
                                                              isize start,
                                                              isize size,
                                                              isize bad_allowed,
                                                              bool leftmost,
                                                              isize cutoff)
{
    // lazy, never scheduled: require() deliberately neither subscribes nor schedules, because the poll loop owns
    // both and would otherwise enqueue a node it is about to run itself
    return cc::make_async_lazy<cc::unit>(sort_async_frame<RangeT, CompareF>{
        .range = range,
        .compare = compare,
        .start = start,
        .size = size,
        .cutoff = cutoff,
        .bad_allowed = i32(bad_allowed),
        .leftmost = leftmost,
    });
}
} // namespace cc::impl

namespace cc
{
/// Sorts [start, start + size) of a virtual range in parallel; the returned async is ready when it is sorted.
///
/// `cutoff` exists so tests can drive the real recursion over a small range — leave it alone in production code.
/// See cc::sort_async for the contract that binds both.
template <class RangeT, class CompareF>
[[nodiscard]] shared_async<unit> sort_async_ex(isize start,
                                               isize size,
                                               RangeT range,
                                               CompareF compare,
                                               isize cutoff = sort_async_default_cutoff)
{
    static_assert(cc::index_swap_range<RangeT>, "cc::sort_async_ex takes an index_swap_range — see "
                                                "cc::as_index_swap_range");
    static_assert(std::is_trivially_copyable_v<RangeT>, "an index_swap_range must be trivially copyable — hold "
                                                        "pointers, not values");
    static_assert(std::is_copy_constructible_v<CompareF>, "cc::sort_async_ex copies the comparator into every "
                                                          "task, so it must be copyable");
    CC_ASSERT(size >= 0, "size must be >= 0");
    CC_ASSERT(cutoff > 0, "cutoff must be > 0");

    return impl::make_sort_async_task(range, compare, start, size, impl::sort_bad_partition_budget(size), true, cutoff);
}

/// Sorts an indexed range ascending, in parallel, and hands back an async that is ready once it is sorted.
///
///   auto const sorted = cc::sort_async(values);
///   auto const next = cc::make_async_lazy([&values] { return use(values); }, sorted);
///
/// Composing into a dependency graph is the point of this, not the raw speedup.
/// The sequential partition caps that at a few times the serial sort however many cores are free.
///
/// THE CALLER OWNS THREE THINGS until the returned async is ready:
///   * `values` must outlive it, must not be resized or reallocated, and must not be read by anyone else —
///     this permutes throughout, not just at the end.
///   * The comparator must be a pure function of its two arguments.
///     It is COPIED into every task, so a stateful one silently counts or caches per task rather than globally.
///   * A comparator that is not a strict weak ordering is worse here than in cc::sort: the unguarded insertion
///     sort would write one position before its subrange, which is a sibling task's pivot — a data race in
///     release rather than a local corruption a CC_ASSERT catches.
///
/// The work runs on whichever scheduler drives the returned async:
///   * created inside a worker      -> starts on that worker's pool immediately;
///   * created on a foreign thread  -> stays cold until required by other async work, submitted through
///     pool.blocking_get / root->schedule_on(pool), or scheduled onto an installed default pool.
/// NEVER SCHEDULED MEANS NEVER SORTED — nothing here blocks or drives itself, and there is deliberately no
/// blocking convenience: pool.blocking_get asserts when called from a worker of that same pool, so a wrapper
/// would be a trap that fires exactly in the composition case it exists to serve.
///
/// Under -DSC_THREADS=OFF the pool keeps its API and drives inline, so this compiles and gives the same answer.
///
/// An errored result (only cancellation can produce one) leaves the range partially sorted; there is no rollback.
template <class RangeT, class CompareF = cc::default_less>
[[nodiscard]] shared_async<unit> sort_async(RangeT& values, CompareF compare = {})
{
    static_assert(cc::indexed_range<RangeT>, "cc::sort_async takes an indexed range");

    return cc::sort_async_ex(0, isize(values.size()), cc::as_index_swap_range(values), cc::move(compare));
}
} // namespace cc
