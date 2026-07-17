#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>

#include <memory>

#if CC_HAS_THREADS
#include <chrono>
#include <thread>
#endif

// cc::async_thread_pool's own tests. Deliberately NOT gated on CC_HAS_THREADS as a whole: the pool exists on
// every platform and falls back to driving graphs inline on the caller (see async_thread_pool.hh), so these
// pin that a pool-shaped API gives pool-shaped ANSWERS with or without threads — which is the only claim the
// fallback makes. Only what genuinely needs a second thread is gated, and each of those says why.
//
// Everything thread-free by construction lives in async-test.cc; this file is about the pool specifically.

using cc::async_context;

namespace
{
// A balanced binary sum-tree: leaves count 1, internal nodes sum both children. Depth d has 2^d leaves, so a
// correct drive returns 2^d — a compact whole-graph workload to run on a pool. The sum alone proves every leaf
// ran exactly once, so there is no leaf-execution counter here (a shared counter would be a data race across
// the pool's worker threads, and redundant).
cc::shared_async<cc::i64> build_sum_tree(int depth)
{
    if (depth == 0)
        return cc::make_async_lazy<cc::i64>([] { return cc::i64(1); });

    auto left = build_sum_tree(depth - 1);
    auto right = build_sum_tree(depth - 1);
    return cc::make_async_lazy([](cc::i64 l, cc::i64 r) { return l + r; }, left, right);
}

} // namespace

TEST("async - a dependency tree drives correctly on a thread pool")
{
    cc::async_thread_pool pool(4);

    int const depth = 10; // 1024 leaves
    auto root = build_sum_tree(depth);

    CHECK(pool.blocking_get(root) == (cc::i64(1) << depth));
}

TEST("async - many independent asyncs fan out across the pool")
{
    cc::async_thread_pool pool(4);

    // one root that sums 64 independent children via a two-phase frame (creates + requires them, then reads
    // them) — the children fan out across the pool's workers
    int const n = 64;
    auto root = cc::make_async_lazy<cc::i64>(
        [n, step = 0,
         kids = cc::vector<cc::shared_async<cc::i64>>()](async_context<cc::i64>& actx) mutable -> cc::async_step_status
        {
            if (step++ == 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    auto k = cc::make_async_lazy([i] { return cc::i64(i); });
                    (void)actx.require(k);
                    kids.push_back(cc::move(k));
                }
                return actx.wait_for_dependencies();
            }
            cc::i64 sum = 0;
            for (auto const& k : kids)
                sum += *k->value_ptr();
            return actx.success(sum);
        });

    CHECK(pool.blocking_get(root) == cc::i64(n) * (n - 1) / 2);
}

// Gated: the whole point is that a SECOND thread supplies the value the first is parked on. With one thread
// the graph parks on a manual node nobody can ever push, which is exactly what blocking_get's is_ready()
// assert reports — a different contract, covered by async-test.cc's no-progress tests.
#if CC_HAS_THREADS
TEST("async - external push from a foreign thread wakes a pool-parked dependent")
{
    cc::async_thread_pool pool(2);
    cc::scoped_default_async_pool as_default(pool); // so the foreign push routes the woken dependent back here

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy([](int x) { return x + 1; }, ext);

    std::thread pusher(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // bias toward p parking first
            ext->push_value(41);
        });

    int const v = pool.blocking_get(p);
    pusher.join();

    CHECK(v == 42);
}
#endif

TEST("async - two pools coexist; each drives its own submitted root")
{
    // Without a task-class system, routing to a specific pool is done by submitting the root to it
    // (blocking_get / schedule_on), not by pinning an affinity. Two independent pools drive independently.
    cc::async_thread_pool pool_a(2);
    cc::async_thread_pool pool_b(1);

    auto ra = cc::make_async_lazy([] { return 7; });
    auto rb = cc::make_async_lazy([] { return 9; });

    CHECK(pool_a.blocking_get(ra) == 7);
    CHECK(pool_b.blocking_get(rb) == 9);
}

TEST("async - installing a second default pool asserts")
{
    cc::async_thread_pool pool_a(1);
    cc::async_thread_pool pool_b(1);

    cc::scoped_default_async_pool as_default(pool_a);
    CHECK_ASSERTS(cc::install_default_async_pool(pool_b)); // a default is already installed
}

TEST("async - two pools coexist and drive independent graphs")
{
    cc::async_thread_pool pool_a(2);
    cc::async_thread_pool pool_b(3);

    auto root_a = build_sum_tree(8); // 256 leaves
    auto root_b = build_sum_tree(9); // 512 leaves

    CHECK(pool_a.blocking_get(root_a) == (cc::i64(1) << 8));
    CHECK(pool_b.blocking_get(root_b) == (cc::i64(1) << 9));
}

TEST("async - stress: many small graphs on a pool")
{
    cc::async_thread_pool pool(4);

    for (int iter = 0; iter < 200; ++iter)
    {
        auto root = build_sum_tree(6); // 64 leaves
        CHECK(pool.blocking_get(root) == (cc::i64(1) << 6));
    }
}

namespace
{
// A value type that counts its own live instances. This is the leak detector, and it has to be the VALUE rather
// than something in the frame: the abandoned entries are overwhelmingly nodes that already RESOLVED (see the
// test below), and resolving destroys the frame. A leaked resolved node still holds its value, so this sees it;
// a frame-side guard would not.
struct counted
{
    static inline cc::atomic<int> live{0};

    cc::i64 v = 0;

    counted() { live.fetch_add(1, cc::memory_order_relaxed); }
    explicit counted(cc::i64 x) : v(x) { live.fetch_add(1, cc::memory_order_relaxed); }
    counted(counted const& o) : v(o.v) { live.fetch_add(1, cc::memory_order_relaxed); }
    counted(counted&& o) noexcept : v(o.v) { live.fetch_add(1, cc::memory_order_relaxed); }
    counted& operator=(counted const&) = delete;
    counted& operator=(counted&&) = delete;
    ~counted() { live.fetch_sub(1, cc::memory_order_relaxed); }
};

// A fork-join tree that spawns its children dynamically, so the work is published BY the workers into their own
// deques -- which is the only way anything ever lands there.
cc::shared_async<counted> spawn_counted_tree(int depth)
{
    return cc::make_async_lazy<counted>(
        [depth, l = cc::shared_async<counted>(),
         r = cc::shared_async<counted>()](async_context<counted>& actx) mutable -> cc::async_step_status
        {
            if (depth == 0)
                return actx.success(counted(1));
            if (l == nullptr)
            {
                l = spawn_counted_tree(depth - 1);
                r = spawn_counted_tree(depth - 1);
                (void)actx.require(l);
                (void)actx.require(r);
                return actx.wait_for_dependencies();
            }
            return actx.success(counted(l->value_ptr()->v + r->value_ptr()->v));
        });
}
} // namespace

TEST("async - destroying a pool releases work abandoned in its deques")
{
    // Abandoning a graph is explicitly allowed (see the pool's lifetime note), and a pool torn down with work
    // still queued must drop those nodes' strong refs -- otherwise the abandoned work pins its whole graph.
    //
    // A queue of handles gets this for free. A queue of RAW pointers, each carrying a hand-held strong count
    // (which is what a lock-free deque requires), only gets it if the destructor drains explicitly. This is the
    // leak the deque rewrite invites, and no other pool test can see it: they all drive to completion first.
    //
    // What actually gets abandoned is worth knowing, because it is not what you would guess. Publish-all-but-one
    // pushes a node's siblings onto the worker's own deque and then drives them inline anyway, so they finish
    // and their queue entries survive as READY NO-OPS that only a later pop would clear. Set _stop while a
    // worker is deep in an inline drive and it leaves the loop with a deque full of exactly those -- resolved
    // nodes, strong refs held, frames long gone. Hence `counted` on the value.
    //
    // Two sizing traps, both hit while writing this: schedule_on() routes to the INJECTION queue (whose entries
    // are handles and free themselves), so the tree must be spawned by the workers to reach a deque at all; and
    // a small tree simply completes, leaving the deques empty and the test green for the wrong reason.
    counted::live.store(0, cc::memory_order_relaxed);
    {
        cc::async_thread_pool pool(4);

        auto root = spawn_counted_tree(16); // 131k nodes: too many to be finished inside the window below
        root->schedule_on(pool);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // pool and root both die here. The destructor cannot interrupt a running frame -- and the eager
        // depth-first drive means "a running frame" is most of the tree -- so it blocks until that unwinds,
        // then the workers exit with whatever is still queued.
    }
    CHECK(counted::live.load(cc::memory_order_relaxed) == 0);
}

TEST("async - the wake protocol survives workers repeatedly draining to empty")
{
    // The wake path has no counter of claimable work: a producer fences, reads _sleepers, and only bumps the
    // epoch if somebody is actually asleep. Losing that race means a task sits in a deque while every worker
    // sleeps -- a HANG, not a wrong answer, so it cannot be caught by checking a value.
    //
    // This drives the window on purpose: many small graphs, submitted one at a time from a foreign thread, so
    // the pool is repeatedly emptied (workers commit to sleeping) and then handed a task (a producer must wake
    // them). A lost wakeup shows up as this test never finishing; dev.py's per-binary timeout reports that
    // rather than hanging the suite forever.
    cc::async_thread_pool pool(4);

    for (int iter = 0; iter < 400; ++iter)
    {
        auto root = build_sum_tree(3); // tiny: finishes fast, so the pool goes idle between iterations
        CHECK(pool.blocking_get(root) == (cc::i64(1) << 3));
    }
}

TEST("async - a pool with one worker still wakes for injected work")
{
    // A 1-worker pool has no peers to steal from, so an injected task is only ever found by the sleeping worker
    // itself -- the wake protocol is the only thing that can deliver it.
    cc::async_thread_pool pool(1);

    for (int iter = 0; iter < 200; ++iter)
    {
        auto root = cc::make_async_lazy([iter] { return cc::i64(iter); });
        CHECK(pool.blocking_get(root) == cc::i64(iter));
    }
}

// ============================================================================
// multi-scheduler correctness — a graph reached from two schedulers at once
// ============================================================================
// Supported and must stay correct; not optimized for. See "Multi-scheduler correctness" in
// libs/base/clean-core/docs/systems/async.md for what is and is not guaranteed.

TEST("async - a singlethreaded_scheduler reports no-progress on a graph parked in a pool")
{
    // The graph is parked on an unpushed manual node inside the pool, so a singlethreaded_scheduler cannot
    // advance it however hard it pumps. That is a report, not an abort: it is not this scheduler's graph to
    // fail. The push then routes the woken dependent to the default pool, which finishes it.
    cc::async_thread_pool pool(1);
    cc::scoped_default_async_pool as_default(pool);

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy([](int x) { return x + 1; }, ext);

    p->schedule_on(pool);

    cc::singlethreaded_scheduler sched;
    CHECK(!sched.try_blocking_get(p).has_value());
    CHECK(!p->is_ready());

    ext->push_value(41);
    CHECK(pool.blocking_get(p) == 42);
}

TEST("async - a subtree shared between a pool and a singlethreaded_scheduler stays correct")
{
    // The real shape of the hybrid case: an outer API that alternates single/multi-threaded over asyncs shared
    // with previous calls, so one subtree is reachable from both schedulers at once. `shared` below is that
    // subtree, driven on the pool; root_st is a dependent driven right here on a singlethreaded_scheduler.
    //
    // Both outcomes for the st driver are legal, and which one happens is a genuine race:
    //   * st drives `shared` inline itself (or finds it already done) and returns the value; or
    //   * `shared` is mid-flight on a worker, so st parks root_st on it. When `shared` completes ON THE POOL
    //     THREAD, route_after_schedule reads the waking thread's scheduler — the pool — so root_st migrates
    //     there and st pumps itself empty: no-progress, and the pool finishes it.
    // A wrong value or an abort is not legal. Correctness only: st never publishes, so it may drag a subtree
    // the pool could have parallelized into single-threaded execution. That is accepted.
    cc::async_thread_pool pool(4);
    cc::scoped_default_async_pool as_default(pool);

    cc::i64 const expected = cc::i64(1) << 6;
    for (int iter = 0; iter < 50; ++iter)
    {
        auto shared = build_sum_tree(6); // 64 leaves, reachable from the pool root and from root_st
        auto root_st = cc::make_async_lazy([](cc::i64 v) { return v; }, shared);

        shared->schedule_on(pool); // the multi-threaded call drives the shared subtree

        cc::singlethreaded_scheduler sched;
        auto const outcome = sched.try_blocking_get(root_st); // the single-threaded call, same subtree
        if (outcome.has_value())
        {
            REQUIRE(outcome.value().has_value());
            CHECK(outcome.value().value() == expected);
        }
        // else: root_st migrated onto the pool mid-drive — no-progress is the correct report, not a failure

        CHECK(pool.blocking_get(root_st) == expected); // resolves once, to the same value, whoever got there
    }
}

TEST("async - a node migrated into a singlethreaded_scheduler is not stranded when it stops driving")
{
    // Regression for the migration-stranding hang. TWO separate roots share a subtree: root_mt is submitted to
    // the pool, root_st is driven on a singlethreaded_scheduler. When st wins the race to drive `shared` inline,
    // `shared` completes on the ST THREAD, so root_mt (parked on it in the pool) is woken there and
    // route_after_schedule enqueues it onto st's queue. st's try_blocking_get returns once root_st is ready,
    // which used to leave root_mt sitting `scheduled` in an abandoned queue — and because schedule_on is
    // idempotent on `scheduled`, the later pool.blocking_get(root_mt) could never reclaim it and hung forever.
    //
    // try_blocking_get now drains its queue before returning (while its worker scope is still bound), settling
    // root_mt into a completed or re-parked state instead of leaving it stranded. This must finish, not hang.
    cc::async_thread_pool pool(4);
    cc::scoped_default_async_pool as_default(pool);

    cc::i64 const expected = cc::i64(1) << 6;
    for (int iter = 0; iter < 50; ++iter)
    {
        auto shared = build_sum_tree(6); // 64 leaves, reachable from both roots
        auto root_mt = cc::make_async_lazy([](cc::i64 v) { return v; }, shared);
        auto root_st = cc::make_async_lazy([](cc::i64 v) { return v; }, shared);

        root_mt->schedule_on(pool); // the multi-threaded call

        cc::singlethreaded_scheduler sched;
        (void)sched.try_blocking_get(root_st); // the single-threaded call, same subtree; may win or migrate

        CHECK(pool.blocking_get(root_mt) == expected); // must not hang: root_mt was drained out of `scheduled`
        CHECK(pool.blocking_get(root_st) == expected);
    }
}
