#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/error/exception.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>


#if CC_HAS_THREADS
#include <chrono>
#include <thread>
#endif

using namespace cc::primitive_defines;

// cc::async_thread_pool's own tests, deliberately NOT gated on CC_HAS_THREADS as a whole.
// The pool exists on every platform and falls back to driving graphs inline on the caller (see async_thread_pool.hh).
// So these pin that a pool-shaped API gives pool-shaped ANSWERS with or without threads, which is the only claim the fallback makes.
// Only what genuinely needs a second thread is gated, and each of those says why.
//
// Everything thread-free by construction lives in async-test.cc; this file is about the pool specifically.

using cc::async_context;

namespace
{
// A balanced binary sum-tree: leaves count 1, internal nodes sum both children.
// Depth d has 2^d leaves, so a correct drive returns 2^d — a compact whole-graph workload to run on a pool.
// The sum alone proves every leaf ran exactly once, so there is no leaf-execution counter: a shared one would be a data race across the pool's workers, and redundant.
cc::shared_async<i64> build_sum_tree(int depth)
{
    if (depth == 0)
        return cc::make_async_lazy<i64>([] { return i64(1); });

    auto left = build_sum_tree(depth - 1);
    auto right = build_sum_tree(depth - 1);
    return cc::make_async_lazy([](i64 l, i64 r) { return l + r; }, left, right);
}

CC_ASYNC_AMBIENT_TAG(pool_tag)

} // namespace

TEST("async - a dependency tree drives correctly on a thread pool", nx::config::no_scheduler)
{
    cc::async_thread_pool pool(4);

    int const depth = 10; // 1024 leaves
    auto root = build_sum_tree(depth);

    CHECK(cc::async_blocking_get_on(pool, root) == (i64(1) << depth));
}

TEST("async - many independent asyncs fan out across the pool", nx::config::no_scheduler)
{
    cc::async_thread_pool pool(4);

    // one root that sums 64 independent children via a two-phase frame (creates + requires them, then reads
    // them) — the children fan out across the pool's workers
    int const n = 64;
    auto root = cc::make_async_lazy<i64>(
        [n, step = 0, kids = cc::vector<cc::shared_async<i64>>()](async_context<i64>& actx) mutable -> cc::async_step_status
        {
            if (step++ == 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    auto k = cc::make_async_lazy([i] { return i64(i); });
                    (void)actx.require(k);
                    kids.push_back(cc::move(k));
                }
                return actx.wait_for_dependencies();
            }
            i64 sum = 0;
            for (auto const& k : kids)
                sum += k->value();
            return actx.success(sum);
        });

    CHECK(cc::async_blocking_get_on(pool, root) == i64(n) * (n - 1) / 2);
}

// Gated: the whole point is that a SECOND thread supplies the value the first is parked on.
// With one thread the graph parks on a manual node nobody can ever push, which is exactly what blocking_get's is_ready() assert reports.
// That is a different contract, covered by async-test.cc's no-progress tests.
#if CC_HAS_THREADS
TEST("async - external push from a foreign thread wakes a pool-parked dependent",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    cc::async_thread_pool pool(2);
    cc::scoped_default_async_scheduler as_default(pool); // so the foreign push routes the woken dependent back here

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy([](int x) { return x + 1; }, ext);

    std::thread pusher(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // bias toward p parking first
            ext->push_value(41);
        });

    int const v = cc::async_blocking_get_on(pool, p);
    pusher.join();

    CHECK(v == 42);
}
#endif

TEST("async - two pools coexist; each drives its own submitted root", nx::config::no_scheduler)
{
    // Without a task-class system, routing to a specific pool means submitting the root to it via blocking_get or schedule_on, not pinning an affinity.
    // Two independent pools drive independently.
    cc::async_thread_pool pool_a(2);
    cc::async_thread_pool pool_b(1);

    auto ra = cc::make_async_lazy([] { return 7; });
    auto rb = cc::make_async_lazy([] { return 9; });

    CHECK(cc::async_blocking_get_on(pool_a, ra) == 7);
    CHECK(cc::async_blocking_get_on(pool_b, rb) == 9);
}

TEST("async - installing a second default pool asserts", nx::config::no_scheduler, exclusive("cc-default-async-pool"))
{
    cc::async_thread_pool pool_a(1);
    cc::async_thread_pool pool_b(1);

    cc::scoped_default_async_scheduler as_default(pool_a);
    CHECK_ASSERTS(cc::install_default_async_scheduler(pool_b)); // a default is already installed
}

TEST("async - two pools coexist and drive independent graphs", nx::config::no_scheduler)
{
    cc::async_thread_pool pool_a(2);
    cc::async_thread_pool pool_b(3);

    auto root_a = build_sum_tree(8); // 256 leaves
    auto root_b = build_sum_tree(9); // 512 leaves

    CHECK(cc::async_blocking_get_on(pool_a, root_a) == (i64(1) << 8));
    CHECK(cc::async_blocking_get_on(pool_b, root_b) == (i64(1) << 9));
}

TEST("async - stress: many small graphs on a pool", nx::config::no_scheduler)
{
    cc::async_thread_pool pool(4);

    for (int iter = 0; iter < 200; ++iter)
    {
        auto root = build_sum_tree(6); // 64 leaves
        CHECK(cc::async_blocking_get_on(pool, root) == (i64(1) << 6));
    }
}

// Gated, helpers and all: the leak this pins can only exist where there ARE deques.
// Without threads the pool has none -- its _queue holds real handles that free themselves when the vector dies (see async_thread_pool.cc).
// So the hand-held strong count that needs an explicit drain never comes into being, and there is no worker to catch mid-drive either.
// The test would pass for a reason that has nothing to do with what it checks.
#if CC_HAS_THREADS
namespace
{
// A value type that counts its own live instances -- the leak detector.
// It has to be the VALUE rather than something in the frame: the abandoned entries are overwhelmingly nodes that already RESOLVED (see the test below), and resolving destroys the frame.
// A leaked resolved node still holds its value, so this sees it where a frame-side guard would not.
struct counted
{
    static inline cc::atomic<int> live = {0};

    i64 v = 0;

    counted() { live.fetch_add(1, cc::memory_order_relaxed); }
    explicit counted(i64 x) : v(x) { live.fetch_add(1, cc::memory_order_relaxed); }
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
            return actx.success(counted(l->value().v + r->value().v));
        });
}
} // namespace

TEST("async - destroying a pool releases work abandoned in its deques", nx::config::no_scheduler)
{
    // Abandoning a graph is explicitly allowed (see the pool's lifetime note), and a pool torn down with work still queued must drop those nodes' strong refs.
    // Otherwise the abandoned work pins its whole graph.
    //
    // A queue of handles gets this for free.
    // A queue of RAW pointers, each carrying a hand-held strong count -- which is what a lock-free deque requires -- only gets it if the destructor drains explicitly.
    // No other pool test can see the difference: they all drive to completion first.
    //
    // What actually gets abandoned is not what you would guess.
    // Publish-all-but-one pushes a node's siblings onto the worker's own deque and then drives them inline anyway.
    // They finish, and their queue entries survive as READY NO-OPS that only a later pop would clear.
    // Set _stop while a worker is deep in an inline drive and it leaves the loop with a deque full of exactly those: resolved nodes, strong refs held, frames long gone.
    // Hence `counted` on the value.
    //
    // Two sizing traps: schedule_on() routes to the INJECTION queue, whose entries are handles that free themselves, so the tree must be spawned by the workers to reach a deque at all.
    // And a small tree simply completes, leaving the deques empty and the test green for the wrong reason.
    counted::live.store(0, cc::memory_order_relaxed);
    {
        cc::async_thread_pool pool(4);

        auto root = spawn_counted_tree(16); // 131k nodes: too many to be finished inside the window below
        root->schedule_on(pool);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // pool and root both die here.
        // The destructor cannot interrupt a running frame -- and the eager depth-first drive means "a running frame" is most of the tree -- so it blocks until that unwinds.
        // The workers then exit with whatever is still queued.
    }
    CHECK(counted::live.load(cc::memory_order_relaxed) == 0);
}
#endif

TEST("async - the wake protocol survives workers repeatedly draining to empty", nx::config::no_scheduler)
{
    // The wake path has no counter of claimable work: a producer fences, reads _sleepers, and only bumps the epoch if somebody is actually asleep.
    // Losing that race means a task sits in a deque while every worker sleeps -- a HANG, not a wrong answer, so it cannot be caught by checking a value.
    //
    // This drives the window on purpose: many small graphs, submitted one at a time from a foreign thread.
    // The pool is repeatedly emptied, so workers commit to sleeping, and then handed a task, so a producer must wake them.
    // A lost wakeup shows up as this test never finishing, and dev.py's per-binary timeout reports that rather than hanging the suite forever.
    cc::async_thread_pool pool(4);

    for (int iter = 0; iter < 400; ++iter)
    {
        auto root = build_sum_tree(3); // tiny: finishes fast, so the pool goes idle between iterations
        CHECK(cc::async_blocking_get_on(pool, root) == (i64(1) << 3));
    }
}

TEST("async - a pool with one worker still wakes for injected work", nx::config::no_scheduler)
{
    // A 1-worker pool has no peers to steal from, so an injected task is only ever found by the sleeping worker
    // itself -- the wake protocol is the only thing that can deliver it.
    cc::async_thread_pool pool(1);

    for (int iter = 0; iter < 200; ++iter)
    {
        auto root = cc::make_async_lazy([iter] { return i64(iter); });
        CHECK(cc::async_blocking_get_on(pool, root) == i64(iter));
    }
}

// ============================================================================
// multi-scheduler correctness — a graph reached from two schedulers at once
// ============================================================================
// Supported and must stay correct; not optimized for.
// See "Multi-scheduler correctness" in libs/base/clean-core/docs/systems/async.md for what is and is not guaranteed.

TEST("async - a singlethreaded_scheduler reports no-progress on a graph parked in a pool",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    // The graph is parked on an unpushed manual node inside the pool, so a singlethreaded_scheduler cannot advance it however hard it pumps.
    // That is a report, not an abort -- it is not this scheduler's graph to fail.
    // The push then routes the woken dependent to the default pool, which finishes it.
    cc::async_thread_pool pool(1);
    cc::scoped_default_async_scheduler as_default(pool);

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy([](int x) { return x + 1; }, ext);

    p->schedule_on(pool);

    cc::singlethreaded_scheduler sched;
    CHECK(!cc::try_async_blocking_get_on(sched, p).has_value());
    CHECK(!p->is_ready());

    ext->push_value(41);
    CHECK(cc::async_blocking_get_on(pool, p) == 42);
}

TEST("async - a subtree shared between a pool and a singlethreaded_scheduler stays correct",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    // The real shape of the hybrid case: an outer API alternating single- and multi-threaded work over asyncs shared with previous calls, so one subtree is reachable from both schedulers at once.
    // `shared` below is that subtree, driven on the pool; root_st is a dependent driven right here on a singlethreaded_scheduler.
    //
    // Both outcomes for the st driver are legal, and which one happens is a genuine race:
    //   * st drives `shared` inline itself, or finds it already done, and returns the value; or
    //   * `shared` is mid-flight on a worker, so st parks root_st on it.
    //     When `shared` completes ON THE POOL THREAD, route_after_schedule reads the waking thread's scheduler — the pool — so root_st migrates there.
    //     st then pumps itself empty: no-progress, and the pool finishes it.
    // A wrong value or an abort is not legal.
    // Correctness only: st never publishes, so it may drag a subtree the pool could have parallelized into single-threaded execution.
    cc::async_thread_pool pool(4);
    cc::scoped_default_async_scheduler as_default(pool);

    i64 const expected = i64(1) << 6;
    for (int iter = 0; iter < 50; ++iter)
    {
        auto shared = build_sum_tree(6); // 64 leaves, reachable from the pool root and from root_st
        auto root_st = cc::make_async_lazy([](i64 v) { return v; }, shared);

        shared->schedule_on(pool); // the multi-threaded call drives the shared subtree

        cc::singlethreaded_scheduler sched;
        auto const outcome = cc::try_async_blocking_get_on(sched, root_st); // the single-threaded call, same subtree
        if (outcome.has_value())
        {
            REQUIRE(outcome.value().has_value());
            CHECK(outcome.value().value() == expected);
        }
        // else: root_st migrated onto the pool mid-drive — no-progress is the correct report, not a failure

        CHECK(cc::async_blocking_get_on(pool, root_st) == expected); // resolves once, to the same value, whoever got there
    }
}

TEST("async - a node migrated into a singlethreaded_scheduler is not stranded when it stops driving",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    // Regression for migration stranding, which is a HANG rather than a wrong answer.
    // TWO separate roots share a subtree: root_mt is submitted to the pool, root_st is driven on a singlethreaded_scheduler.
    // When st wins the race to drive `shared` inline, `shared` completes on the ST THREAD, so root_mt — parked on it in the pool — is woken there and route_after_schedule enqueues it onto st's queue.
    // st's try_blocking_get returns once root_st is ready, which would leave root_mt sitting `scheduled` in an abandoned queue.
    // schedule_on is idempotent on `scheduled`, so the later cc::async_blocking_get_on(pool, root_mt) could never reclaim it.
    //
    // try_blocking_get drains its queue before returning, with its worker scope still bound, settling root_mt into a completed or re-parked state.
    // This test must finish, not hang.
    cc::async_thread_pool pool(4);
    cc::scoped_default_async_scheduler as_default(pool);

    i64 const expected = i64(1) << 6;
    for (int iter = 0; iter < 50; ++iter)
    {
        auto shared = build_sum_tree(6); // 64 leaves, reachable from both roots
        auto root_mt = cc::make_async_lazy([](i64 v) { return v; }, shared);
        auto root_st = cc::make_async_lazy([](i64 v) { return v; }, shared);

        root_mt->schedule_on(pool); // the multi-threaded call

        cc::singlethreaded_scheduler sched;
        (void)cc::try_async_blocking_get_on(sched, root_st); // the single-threaded call, same subtree; may win or migrate

        CHECK(cc::async_blocking_get_on(pool, root_mt) == expected); // must not hang: root_mt was drained out of `scheduled`
        CHECK(cc::async_blocking_get_on(pool, root_st) == expected);
    }
}

#if CC_HAS_THREADS
TEST("async - a node woken across threads still runs under its own ambient context",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    // Needs real threads: the point is that the context comes from the node's own arm and never from the worker
    // that happens to pick it up, so the dependent must be re-polled somewhere other than where it parked.
    cc::async_thread_pool pool(4);
    cc::scoped_default_async_scheduler as_default(pool);

    int scope_value = 7;
    auto gate = cc::make_async_manual<i64>();
    auto dependent = cc::make_async_lazy<i64>(
        [](i64) -> i64
        {
            auto* const v = cc::async_ambient_lookup(pool_tag());
            return v == nullptr ? 0 : *static_cast<int*>(v);
        },
        gate);

    {
        cc::async_ambient_scope const s(pool_tag(), &scope_value);
        dependent->schedule_on(pool); // parks on the manual node, on some worker
    }
    // The scope is gone from THIS thread, and it was never installed on any worker.

    std::thread pusher([&] { gate->push_value(1); }); // completes it from a thread with nothing bound at all
    pusher.join();

    CHECK(cc::async_blocking_get_on(pool, dependent) == scope_value);
}

TEST("async - a throwing frame on a worker does not take the process down", nx::config::no_scheduler)
{
    // Needs real threads: a worker loop has no handler of its own, so an escaping exception would leave worker_main via std::thread and terminate the process outright.
    // Containment happens per node, in poll.
    cc::async_thread_pool pool(4);

    auto root = cc::make_async_lazy<i64>(
        [](async_context<i64>& ctx) -> cc::async_step_status
        {
            throw std::runtime_error("worker boom");
            return ctx.success(0); // unreachable
        });

    auto const r = cc::try_async_blocking_get_on(pool, root);
    REQUIRE(r.has_value()); // the pool completed it, on the failure channel
    REQUIRE(r.value().has_error());
    CHECK(r.value().error().underlying().to_string().contains("worker boom"));
}

TEST("async - a work item stolen by a parked participant runs under its own context, not the stealer's",
     nx::config::no_scheduler,
     exclusive("cc-default-async-pool"))
{
    // A blocking get parks INSIDE an ambient scope and steals while parked, so whatever it picks up would inherit that
    // scope if a dequeued item were treated like an inline-driven dependency.
    // It must not be: a node reaching a queue always carries its own token, so a null one there means "no context".
    // Inheriting instead cross-attributes unrelated work to whichever logical task happened to be blocked.
    //
    // The pool's one worker is pinned inside `hog` for the whole test, so the parked participant is the only thread
    // left that can run `victim` — the steal is forced rather than raced.
    cc::async_thread_pool pool(1);
    cc::scoped_default_async_scheduler as_default(
        pool); // the pusher thread has nothing bound, and resolving `gate` routes a continuation

    cc::atomic<bool> hog_running = {false};
    cc::atomic<bool> release_hog = {false};
    auto hog = cc::make_async_lazy<i64>(
        [&]() -> i64
        {
            hog_running.store(true, cc::memory_order_release);
            while (!release_hog.load(cc::memory_order_acquire))
                std::this_thread::yield();
            return 0;
        });
    hog->schedule_on(pool);
    while (!hog_running.load(cc::memory_order_acquire))
        std::this_thread::yield();

    int scope_value = 7;
    cc::atomic<int> observed = {-1}; // what `victim` saw: -1 not run, 0 no context, 7 the stealer's

    auto gate = cc::make_async_manual<i64>();
    auto root = cc::make_async_lazy([](i64 v) { return v; }, gate);

    auto victim = cc::make_async_lazy<i64>(
        [&]() -> i64
        {
            auto* const v = cc::async_ambient_lookup(pool_tag());
            observed.store(v == nullptr ? 0 : *static_cast<int*>(v), cc::memory_order_release);
            return 1;
        });

    // Nothing is bound on this thread, so `victim` reaches the queue with a null token — the case that inherits.
    // Releasing the root only once `victim` has run is what makes the order the test claims the order it gets.
    std::thread pusher(
        [&]
        {
            victim->schedule_on(pool);
            while (observed.load(cc::memory_order_acquire) < 0)
                std::this_thread::yield();
            gate->push_value(1);
        });

    {
        cc::async_ambient_scope const s(pool_tag(), &scope_value);
        CHECK(cc::async_blocking_get_on(pool, root) == 1);
    }
    pusher.join();

    CHECK(observed.load(cc::memory_order_acquire) == 0);

    release_hog.store(true, cc::memory_order_release);
    (void)cc::async_blocking_get_on(pool, hog); // settle it before the pool goes away
}
#endif
