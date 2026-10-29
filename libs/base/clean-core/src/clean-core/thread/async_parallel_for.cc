#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <clean-core/thread/atomic.hh>

using namespace cc::primitive_defines;

// The loop's machinery, measured against the alternatives in libs/base/clean-core/docs/benchmarks/async-benchmark.md, "Parallel loops".
//
// One helper per thread the scheduler has, each owning an even slice of [0, n).
// A helper pops a grain off its own slice's front, and once that is empty steals the back half of another slice, or the whole of one a grain long or less.
// Slices rather than one shared counter because a claim then touches a line only its owner writes, until a thief comes:
// a shared counter at grain 1 over 5 ns bodies ran 8x slower than serial, slices 3.5x faster than it.
//
// Helpers start through the scheduler's broadcast slot when it has one, so every idle thread joins without a node of its own.
// The loop then completes by a countdown of indices: whoever finishes the last one pushes a manual node the loop's frame awaits,
// and closing the slot waits for the joiners still leaving.
//
// Otherwise, or when an outer loop holds the slot, helpers start as a tree of nodes: each schedules two more as it begins, and awaits them before it resolves.
// A flat start, the caller scheduling every helper, put ~0.45 us between consecutive helpers, so the last of 31 began 24 us late.
// The loop's frame is the tree's root, so it settles only once every helper has: none outlives the loop, still carrying its context.
// A helper that starts after the work is gone finds nothing to claim and resolves at once.

namespace
{
/// A slice [begin, end) packed into one word, so its owner and a thief race on a single CAS.
/// While a slice is non-empty its begin only grows and its end only shrinks.
/// A refill holds indices nobody has claimed, disjoint from everything the slice held before, so a stale CAS cannot succeed: no ABA.
u64 pack(isize begin, isize end)
{
    return (u64(u32(begin)) << 32) | u64(u32(end));
}
isize begin_of(u64 v)
{
    return isize(v >> 32);
}
isize end_of(u64 v)
{
    return isize(u32(v));
}

struct slot
{
    alignas(64) cc::atomic<u64> range = {0};

    slot() = default;
    slot(slot&&) noexcept {} // only for the vector's one sizing, before any helper exists
};

struct job
{
    cc::unique_function<void(isize, isize)> body;
    isize n = 0;
    isize grain = 1;
    int helpers = 1;
    bool started = false; // touched only by the loop's own frame
    bool tree = false;    // helpers start as a tree of nodes, not through a broadcast
    cc::async_scheduler* sched = nullptr;
    cc::async_scheduler* broadcast = nullptr; // the scheduler whose slot this loop holds, closed by the frame
    cc::vector<slot> slots;
    cc::vector<cc::shared_async<cc::unit>> nodes; // tree helpers by index, each set by its parent before it is scheduled
    cc::shared_async<cc::unit> done;              // broadcast only: pushed by the countdown

    cc::atomic<isize> remaining
        = {0}; // broadcast only: indices not yet run or dropped; whoever brings it to 0 pushes done
    cc::atomic<int> next_joiner = {1}; // the helper index a broadcast joiner takes; the frame's own thread is 0
    cc::atomic<u32> next_victim = {0}; // rotates where thieves start looking
    cc::atomic<bool> failed = {false};
    cc::string failure; // written once, by whoever set failed; read by the frame after done

    explicit job(cc::unique_function<void(isize, isize)> b) : body(cc::move(b)) {}

    bool claim(int k, isize& begin, isize& end)
    {
        // our own slice's front: uncontended unless a thief is at it
        auto& own = slots[k].range;
        auto v = own.load(cc::memory_order_acquire);
        while (begin_of(v) < end_of(v))
        {
            auto const b = begin_of(v);
            auto const nb = cc::min(b + grain, end_of(v));
            if (own.compare_exchange_weak(v, pack(nb, end_of(v)), cc::memory_order_acq_rel, cc::memory_order_acquire))
            {
                begin = b;
                end = nb;
                return true;
            }
        }

        // then the others, from a rotating start so thieves spread; one empty pass over all of them ends the helper
        auto const start
            = int((u32(k) * 2654435761u + next_victim.fetch_add(1, cc::memory_order_relaxed)) % u32(helpers));
        for (auto i = 0; i < helpers; ++i)
        {
            auto const j = (start + i) % helpers;
            if (j == k)
                continue;
            auto& victim = slots[j].range;
            auto w = victim.load(cc::memory_order_acquire);
            while (begin_of(w) < end_of(w))
            {
                auto const b = begin_of(w);
                auto const e = end_of(w);
                if (e - b <= grain)
                {
                    if (victim.compare_exchange_weak(w, pack(b, b), cc::memory_order_acq_rel, cc::memory_order_acquire))
                    {
                        begin = b;
                        end = e;
                        return true;
                    }
                    continue;
                }
                auto const mid = b + (e - b) / 2;
                if (victim.compare_exchange_weak(w, pack(b, mid), cc::memory_order_acq_rel, cc::memory_order_acquire))
                {
                    // run the stolen half's first grain now, and publish the rest as our slice: it is empty, so no thief CASes it
                    begin = mid;
                    end = cc::min(mid + grain, e);
                    own.store(pack(end, e), cc::memory_order_release);
                    return true;
                }
            }
        }
        return false;
    }

    /// Empties every slice and returns how many indices that dropped.
    /// Chunks already claimed still run and count themselves; a thief may still publish a half it took before, and runs it.
    isize drop_unclaimed()
    {
        auto dropped = isize(0);
        for (auto& s : slots)
        {
            auto const v = s.range.exchange(pack(0, 0), cc::memory_order_acq_rel);
            dropped += cc::max(isize(0), end_of(v) - begin_of(v));
        }
        return dropped;
    }

    void count_down(isize finished)
    {
        if (tree)
            return; // the tree's root awaits the helpers themselves
        // a helper that finished nothing must not count down: fetch_sub(0) would read the final 0 a second time
        if (finished > 0 && remaining.fetch_sub(finished, cc::memory_order_acq_rel) == finished)
            done->push_value(cc::unit());
    }
};

void run_claims(job& j, int k)
{
    auto finished = isize(0);
    auto begin = isize(0);
    auto end = isize(0);
    while (j.claim(k, begin, end))
    {
#ifdef CC_HAS_CPP_EXCEPTIONS
        try
        {
            j.body(begin, end);
        }
        catch (...)
        {
            // The first failure is the loop's; the chunk counts as finished, and what nobody claimed yet is dropped.
            auto expected = false;
            if (j.failed.compare_exchange_strong(expected, true, cc::memory_order_acq_rel))
                j.failure = cc::impl::async_describe_current_exception();
            finished += end - begin + j.drop_unclaimed();
            continue;
        }
#else
        j.body(begin, end);
#endif
        finished += end - begin;
    }
    j.count_down(finished);
}

void launch_children(cc::shared_ptr<job> const& j, int k);

/// Helper k's children in the tree, awaited: true once both are ready.
bool children_ready(job& j, int k, cc::async_context<cc::unit>& actx)
{
    auto ready = true;
    for (auto const c : {2 * k + 1, 2 * k + 2})
        if (c < j.helpers && !actx.require(j.nodes[c]))
            ready = false;
    return ready;
}

void launch_children(cc::shared_ptr<job> const& j, int k)
{
    for (auto const c : {2 * k + 1, 2 * k + 2})
    {
        if (c >= j->helpers)
            continue;
        // 16 B of captures, inside the node's inline slot; they let go of the job as the node resolves
        j->nodes[c] = cc::make_async_lazy<cc::unit>(
            [j, c, ran = false](cc::async_context<cc::unit>& actx) mutable -> cc::async_step_status
            {
                if (!ran)
                {
                    ran = true;
                    launch_children(j, c);
                    run_claims(*j, c);
                }
                if (!children_ready(*j, c, actx))
                    return actx.wait_for_dependencies();
                return actx.success(cc::unit());
            });
        j->nodes[c]->schedule_on(*j->sched);
    }
}

/// A broadcast joiner takes the next helper index, or leaves once every helper has one.
/// The frame closes the slot only after done, so ctx is alive for every joiner.
void join_broadcast(void* ctx)
{
    auto& j = *static_cast<job*>(ctx);
    auto const k = j.next_joiner.fetch_add(1, cc::memory_order_relaxed);
    if (k < j.helpers)
        run_claims(j, k);
}
} // namespace

cc::shared_async<cc::unit> cc::impl::async_parallel_for_ranges(isize n,
                                                               unique_function<void(isize, isize)> body,
                                                               parallel_for_options options)
{
    CC_ASSERT(n >= 0, "a parallel loop over a negative range");
    CC_ASSERT(n < (isize(1) << 32), "a parallel loop's slices pack two 32-bit bounds into one word");
    CC_ASSERT(options.grain >= 0, "a negative grain");

    auto j = cc::make_shared<job>(cc::move(body));
    j->n = n;
    j->grain = options.grain;

    // 16 B of captures, inside the node's inline slot
    return cc::make_async_lazy<cc::unit>(
        [j = cc::move(j)](async_context<cc::unit>& actx) -> async_step_status
        {
            if (!j->started)
            {
                j->started = true;
                auto* sched = async_scheduler::current_or_null();
                if (sched == nullptr)
                    sched = async_scheduler::compute_or_null();
                auto const parallelism = sched == nullptr ? 1 : sched->parallelism();
                if (j->grain == 0)
                    j->grain = cc::max(isize(1), j->n / (64 * isize(parallelism)));
                j->helpers = int(cc::min((j->n + j->grain - 1) / j->grain, isize(parallelism)));

                if (j->helpers <= 1 || !sched->has_steal_capable_peers)
                {
                    // inline; a throwing body fails this node through the poll loop's own containment
                    if (j->n > 0)
                        j->body(0, j->n);
                    j->body = {};
                    return actx.success(cc::unit());
                }

                j->sched = sched;
                j->remaining.store(j->n, cc::memory_order_relaxed);
                j->slots.resize_to_defaulted(j->helpers);
                for (auto k = 0; k < j->helpers; ++k)
                    j->slots[k].range.store(pack(j->n * k / j->helpers, j->n * (k + 1) / j->helpers),
                                            cc::memory_order_relaxed);

                j->done = make_async_manual<cc::unit>(); // before the slot opens: a joiner may finish the loop at once
                if (sched->try_open_broadcast({.run = &join_broadcast, .ctx = j.get()}))
                {
                    j->broadcast = sched;
                }
                else
                {
                    j->tree = true;
                    j->nodes.resize_to_defaulted(j->helpers);
                    launch_children(j, 0);
                }
                run_claims(*j, 0); // this thread is helper 0
            }

            if (j->tree ? !children_ready(*j, 0, actx) : !actx.require(j->done))
                return actx.wait_for_dependencies();

            if (j->broadcast != nullptr)
                j->broadcast->close_broadcast();
            j->body = {}; // no claim succeeds any more, so nobody calls it again
            if (j->failed.load(cc::memory_order_acquire))
                return actx.error(cc::any_error(cc::move(j->failure)));
            return actx.success(cc::unit());
        });
}
