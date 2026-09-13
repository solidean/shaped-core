#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_mutex.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Async mutexes: contention parks the waiting node, and release hands the lock to the oldest live waiter.
// Most of these run the same way with and without threads — a holder suspended across an await is contention on one thread too.

namespace
{
bool same_order(cc::vector<int> const& got, std::initializer_list<int> expected)
{
    if (got.size() != isize(expected.size()))
        return false;
    auto i = isize(0);
    for (auto const e : expected)
        if (got[i++] != e)
            return false;
    return true;
}

/// Runs everything the test's bound single-threaded scheduler has queued.
void drain()
{
    while (cc::async_scheduler::current().try_run_one())
    {
    }
}

/// A coroutine that takes the lock, records entry, suspends on `gate` while holding it, records exit.
cc::shared_async<cc::unit> hold_across(cc::async_mutex<cc::vector<int>>& m, cc::shared_async<cc::unit> gate, int id)
{
    auto guard = co_await m.lock();
    guard->push_back(id);
    co_await gate;
    guard->push_back(-id);
    co_return;
}
} // namespace

TEST("async mutex - an uncontended lock is taken without suspending and released by its guard", nx::config::singlethreaded)
{
    cc::async_mutex<int> m(1);
    {
        auto g = m.try_lock();
        REQUIRE(g.has_value());
        *g.value() += 1;
        CHECK(!m.try_lock().has_value()); // held
    }
    auto const g2 = m.try_lock();
    REQUIRE(g2.has_value());
    CHECK(*g2.value() == 2);
}

TEST("async mutex - a holder suspended across an await excludes the next node, which runs after it in order",
     nx::config::singlethreaded)
{
    cc::async_mutex<cc::vector<int>> m;
    auto const gate_a = cc::make_async_manual<cc::unit>();
    auto const gate_b = cc::make_async_manual<cc::unit>();

    auto const a = cc::async_start(hold_across(m, gate_a, 1));
    drain(); // a holds the lock and parks on its gate
    auto const b = cc::async_start(hold_across(m, gate_b, 2));
    drain(); // b parks on the lock

    gate_b->push_value({}); // b is not holding the lock yet, so releasing its gate changes nothing
    drain();
    CHECK(!b->is_ready());

    gate_a->push_value({});
    drain();
    CHECK(a->is_ready());
    CHECK(b->is_ready());

    auto const g = m.try_lock();
    REQUIRE(g.has_value());
    CHECK(same_order(*g.value(), {1, -1, 2, -2})); // never interleaved
}

TEST("async mutex - waiters are served in arrival order", nx::config::singlethreaded)
{
    cc::async_mutex<cc::vector<int>> m;
    auto held = m.try_lock();
    REQUIRE(held.has_value());

    cc::vector<cc::shared_async<cc::unit>> waiters;
    for (auto i = 0; i < 5; ++i)
    {
        waiters.push_back(cc::async_start(
            [](cc::async_mutex<cc::vector<int>>& mm, int id) -> cc::shared_async<cc::unit>
            {
                auto guard = co_await mm.lock();
                guard->push_back(id);
                co_return;
            }(m, i)));
        drain(); // this one queues on the lock before the next is started: the scheduler's own queue is LIFO
    }

    held.value().unlock();
    for (auto const& w : waiters)
        (void)cc::async_blocking_get(w);

    auto const g = m.try_lock();
    REQUIRE(g.has_value());
    CHECK(same_order(*g.value(), {0, 1, 2, 3, 4}));
}

TEST("async mutex - a waiter dropped before its turn is skipped, and one dropped after it gives the lock back",
     nx::config::singlethreaded)
{
    cc::async_mutex<int> m(0);
    auto held = m.try_lock();
    REQUIRE(held.has_value());

    auto dropped_early = m.lock_async(); // queued
    auto dropped_late = m.lock_async();  // queued behind it
    auto kept = m.lock_async();
    CHECK(!dropped_early->is_ready());

    dropped_early = nullptr;
    held.value().unlock();

    // The dead head was skipped: the second waiter holds the lock now, untaken inside its grant.
    REQUIRE(dropped_late->is_ready());
    CHECK(!kept->is_ready());

    dropped_late = nullptr; // its guard dies with the grant, and hands the lock on
    REQUIRE(kept->is_ready());
    auto guard = kept->take_value();
    *guard = 7;
    guard.unlock();
    CHECK(m.try_lock().has_value());
}

TEST("async mutex - a raw frame takes the lock through lock_async", nx::config::singlethreaded)
{
    cc::async_mutex<int> m(40);
    auto held = m.try_lock();
    REQUIRE(held.has_value());

    auto const node = cc::make_async_lazy<int>(
        [&m, grant = cc::shared_async<cc::async_mutex_guard<int>>()](
            cc::async_context<int>& ctx) mutable -> cc::async_step_status
        {
            if (grant == nullptr)
                grant = m.lock_async();
            if (!ctx.require(grant))
                return ctx.wait_for_dependencies();
            auto guard = grant->take_value();
            *guard += 2;
            return ctx.success(*guard);
        });
    node->schedule();
    drain(); // the frame queues on the lock and parks
    CHECK(!node->is_ready());

    held.value().unlock();
    CHECK(cc::async_blocking_get(node) == 42);
}

TEST("async shared mutex - readers share, a writer excludes them, and a waiting writer holds back later readers",
     nx::config::singlethreaded)
{
    cc::async_shared_mutex<int> m(0);

    auto r1 = m.try_lock_shared();
    auto r2 = m.try_lock_shared();
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(!m.try_lock().has_value());

    auto const writer = m.lock_async();             // waits for both readers
    auto const late_reader = m.lock_shared_async(); // arrives after a waiting writer: waits behind it
    CHECK(!m.try_lock_shared().has_value());

    r1.value().unlock();
    CHECK(!writer->is_ready());
    r2.value().unlock();
    REQUIRE(writer->is_ready());
    CHECK(!late_reader->is_ready());

    {
        auto w = writer->take_value();
        *w = 5;
    }
    REQUIRE(late_reader->is_ready());
    CHECK(*late_reader->take_value() == 5);
}

TEST("async semaphore - holds its count, and a large request waits at the head of the line", nx::config::singlethreaded)
{
    cc::async_semaphore s(3);
    auto a = s.try_acquire(2);
    REQUIRE(a.has_value());

    auto const big = s.acquire_async(3);   // needs everything
    auto const small = s.acquire_async(1); // would fit now, but may not pass the head of the queue
    CHECK(!big->is_ready());
    CHECK(!small->is_ready());

    a.value().release();
    REQUIRE(big->is_ready());
    CHECK(!small->is_ready());

    {
        auto const permit = big->take_value();
    }
    CHECK(small->is_ready());
}

#if CC_HAS_THREADS
TEST("async mutex - exclusion holds under contention on a pool", nx::config::no_scheduler)
{
    cc::async_thread_pool pool(4);
    cc::async_mutex<i64> m(0);
    cc::atomic<int> inside = {0};
    cc::atomic<int> overlaps = {0};

    auto const worker = [&](int n) -> cc::shared_async<cc::unit>
    {
        for (auto i = 0; i < n; ++i)
        {
            auto guard = co_await m.lock();
            if (inside.fetch_add(1) != 0)
                overlaps.fetch_add(1);
            *guard += 1;
            co_await cc::async_yield(); // hold across a suspension, so other workers genuinely queue
            inside.fetch_sub(1);
        }
        co_return;
    };

    cc::vector<cc::shared_async<cc::unit>> nodes;
    for (auto i = 0; i < 16; ++i)
    {
        nodes.push_back(worker(50));
        nodes.back()->schedule_on(pool);
    }
    for (auto const& n : nodes)
        (void)cc::async_blocking_get_on(pool, n);

    CHECK(overlaps.load() == 0);
    auto const g = m.try_lock();
    REQUIRE(g.has_value());
    CHECK(*g.value() == 16 * 50);
}
#endif
