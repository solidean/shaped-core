#include <clean-core/common/macros.hh> // CC_HAS_THREADS, CC_HAS_CPP_EXCEPTIONS
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/exception.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/spin.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// cc::async_parallel_for runs every index exactly once, however the range, grain and scheduler fall.
// The pool's broadcast slot and idle linger, which the loop rides on, are pinned at the bottom on pools the tests own.

namespace
{
/// cc::vector needs movable elements; it is sized once, before any loop touches it
struct hit
{
    cc::atomic<int> count = {0};

    hit() = default;
    hit(hit&&) noexcept {}
};

/// Indices hit a number of times other than once.
isize wrong_hits(cc::vector<hit> const& hits)
{
    auto wrong = isize(0);
    for (auto const& h : hits)
        wrong += h.count.load(cc::memory_order_relaxed) != 1;
    return wrong;
}

cc::shared_async<cc::unit> check_ranges_and_grains()
{
    for (auto const n : {isize(0), isize(1), isize(7), isize(1000), isize(100003)})
        for (auto const grain : {isize(0), isize(1), isize(16), isize(1024)})
        {
            auto hits = cc::vector<hit>();
            hits.resize_to_defaulted(n);
            co_await cc::async_parallel_for(n, [&](isize i) { hits[i].count.fetch_add(1, cc::memory_order_relaxed); },
                                            {.grain = grain});
            CHECK(wrong_hits(hits) == 0);

            auto chunk_hits = cc::vector<hit>();
            chunk_hits.resize_to_defaulted(n);
            co_await cc::async_parallel_for(n,
                                            [&](isize begin, isize end)
                                            {
                                                CHECK(begin < end);
                                                for (auto i = begin; i < end; ++i)
                                                    chunk_hits[i].count.fetch_add(1, cc::memory_order_relaxed);
                                            },
                                            {.grain = grain});
            CHECK(wrong_hits(chunk_hits) == 0);
        }
}
} // namespace

ASYNC_TEST("async_parallel_for - every index exactly once on a pool", own_pool(7))
{
    co_await check_ranges_and_grains();
}

ASYNC_TEST("async_parallel_for - every index exactly once on a one-worker pool", own_pool(1))
{
    co_await check_ranges_and_grains();
}

ASYNC_TEST("async_parallel_for - runs inline, in order, on a single-threaded scheduler", singlethreaded)
{
    co_await check_ranges_and_grains();

    auto order = cc::vector<isize>();
    co_await cc::async_parallel_for(100, [&](isize i) { order.push_back(i); });
    auto in_order = order.size() == 100;
    for (auto i = isize(0); i < order.size(); ++i)
        in_order = in_order && order[i] == i;
    CHECK(in_order);
}

ASYNC_TEST("async_parallel_for - concurrent loops share the pool, the broadcast slot held by one", own_pool(7))
{
    auto hits = cc::vector<hit>();
    hits.resize_to_defaulted(4 * 20000);
    auto const body = [&](isize offset)
    { return [&hits, offset](isize i) { hits[offset + i].count.fetch_add(1, cc::memory_order_relaxed); }; };
    auto const a = cc::async_parallel_for(20000, body(0));
    auto const b = cc::async_parallel_for(20000, body(20000));
    auto const c = cc::async_parallel_for(20000, body(40000));
    auto const d = cc::async_parallel_for(20000, body(60000));
    co_await cc::async_all(a, b, c, d);
    CHECK(wrong_hits(hits) == 0);
}

ASYNC_TEST("async_parallel_for - with the broadcast slot held, helpers run as a tree and none outlives its loop",
           own_pool(7))
{
    // nexus fails the test if any helper still carries its context once the body returned
    auto& sched = cc::async_scheduler::current();
    auto const held = sched.try_open_broadcast({.run = [](void*) {}, .ctx = nullptr});
    for (auto round = 0; round < 200; ++round)
    {
        auto hits = cc::vector<hit>();
        hits.resize_to_defaulted(3000);
        co_await cc::async_parallel_for(3000, [&](isize i) { hits[i].count.fetch_add(1, cc::memory_order_relaxed); });
        CHECK(wrong_hits(hits) == 0);
    }
    if (held)
        sched.close_broadcast();
}

ASYNC_TEST("async_parallel_for - a held loop owns its body", own_pool(3))
{
    // the lambda temporary dies at the end of this statement; the loop runs later, from its own copy
    auto const loop = cc::async_parallel_for(
        1000, [count = cc::vector<int>::create_filled(1000, 0)](isize i) mutable { count[i] += 1; });
    co_await loop;
    CHECK(loop->is_ready());
}

#ifdef CC_HAS_CPP_EXCEPTIONS
ASYNC_TEST("async_parallel_for - a throwing body fails the loop, once every running chunk finished", own_pool(7))
{
    auto running = cc::atomic<int>(0);
    auto ran = cc::atomic<isize>(0);
    auto const loop = cc::async_parallel_for(100000,
                                             [&](isize i)
                                             {
                                                 running.fetch_add(1);
                                                 if (i == 5000)
                                                 {
                                                     running.fetch_sub(1);
                                                     throw cc::exception(cc::string("index 5000 refuses"));
                                                 }
                                                 ran.fetch_add(1);
                                                 running.fetch_sub(1);
                                             },
                                             {.grain = 64});
    auto const r = co_await cc::async_as_result(loop);
    CHECK(r.has_error());
    CHECK(running.load() == 0); // nothing of the body still runs once the loop settled
    CHECK(ran.load() < 100000);
}

ASYNC_TEST("async_parallel_for - a throwing body inline fails the loop too", singlethreaded)
{
    auto const loop = cc::async_parallel_for(10,
                                             [](isize i)
                                             {
                                                 if (i == 3)
                                                     throw cc::exception(cc::string("index 3 refuses"));
                                             });
    auto const r = co_await cc::async_as_result(loop);
    CHECK(r.has_error());
}
#endif

// --- the pool's side: parallelism, the broadcast slot, linger ---------------------------------------------
// These own their pools, since the knobs and the slot are what they test; blocking on them is the subject.

TEST("async_thread_pool - parallelism counts the driving thread", no_scheduler)
{
    cc::singlethreaded_scheduler single;
    CHECK(single.parallelism() == 1);
    cc::async_thread_pool pool(3);
#if CC_HAS_THREADS
    CHECK(pool.parallelism() == 4);
#else
    CHECK(pool.parallelism() == 1);
#endif
}

#if CC_HAS_THREADS
TEST("async_thread_pool - a broadcast is joined by idle workers once each, and held by one opener", no_scheduler)
{
    cc::async_thread_pool pool(3);
    struct counter
    {
        cc::atomic<int> joins = {0};
    } c;
    auto const job
        = cc::async_broadcast_job{.run = [](void* ctx) { static_cast<counter*>(ctx)->joins.fetch_add(1); }, .ctx = &c};
    CHECK(pool.try_open_broadcast(job));
    CHECK(!pool.try_open_broadcast(job));

    // the workers are idle or asleep; opening woke them, so they join within moments
    auto const until = cc::current_time_steady_secs() + 5.0;
    while (c.joins.load() < 3 && cc::current_time_steady_secs() < until)
        cc::spin_pause();
    pool.close_broadcast();
    CHECK(c.joins.load() == 3);

    // closed: a new job may open, and the old one is joined no more
    CHECK(pool.try_open_broadcast(job));
    pool.close_broadcast();
}

TEST("async_parallel_for - a body blocking on a nested loop does not deadlock the pool", no_scheduler)
{
    cc::async_thread_pool pool(7);
    auto total = cc::atomic<isize>(0);
    auto const outer = cc::async_parallel_for(
        64, [&](isize)
        { cc::async_blocking_get(cc::async_parallel_for(1000, [&](isize) { total.fetch_add(1); }, {.grain = 10})); },
        {.grain = 1});
    cc::async_blocking_get_on(pool, outer);
    CHECK(total.load() == 64 * 1000);
}

TEST("async_parallel_for - loops stay correct on a lingering pool", no_scheduler)
{
    cc::async_thread_pool pool(7);
    pool.set_idle_linger_us(50);
    CHECK(pool.idle_linger_us() == 50);
    for (auto round = 0; round < 50; ++round)
    {
        auto hits = cc::vector<hit>();
        hits.resize_to_defaulted(5000);
        cc::async_blocking_get_on(pool, cc::async_parallel_for(5000, [&](isize i) { hits[i].count.fetch_add(1); }));
        CHECK(wrong_hits(hits) == 0);
    }
}
#else
TEST("async_thread_pool - without threads nobody joins a broadcast", no_scheduler)
{
    cc::async_thread_pool pool(3);
    CHECK(!pool.try_open_broadcast({.run = [](void*) {}, .ctx = nullptr}));
}
#endif
