#include <clean-core/container/span.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

ASYNC_TEST("async backlog - an empty backlog settles at once")
{
    cc::async_backlog const backlog;
    co_await cc::async_settled(backlog.settled());
    CHECK(backlog.outstanding_count() == 0);
}

ASYNC_TEST("async backlog - settled waits for a tracked node to settle")
{
    cc::async_backlog backlog;
    auto const work = cc::make_async_manual<int>();
    backlog.track(work);
    CHECK(backlog.outstanding_count() == 1);

    auto const settled = cc::async_start(backlog.settled());
    CHECK(!settled->is_ready()); // the work is external_pending, so no interleaving can settle it early

    work->push_value(7);
    co_await cc::async_settled(settled);
    CHECK(backlog.outstanding_count() == 0);
}

ASYNC_TEST("async backlog - a failed node is settled work, and settled itself never fails")
{
    cc::async_backlog backlog;
    auto const work = cc::make_async_manual<int>();
    backlog.track(work);

    auto const settled = cc::async_start(backlog.settled());
    work->push_error(cc::async_error::make_error(cc::any_error("boom")));

    co_await cc::async_settled(settled);
    CHECK(settled->try_value() != nullptr);
}

ASYNC_TEST("async backlog - start schedules a cold node and hands the same handle back")
{
    cc::async_backlog backlog;
    auto const lazy = cc::make_async_lazy([] { return 42; });
    auto const started = backlog.start(lazy);

    CHECK(started == lazy);
    CHECK(!started->is_cold());

    co_await cc::async_settled(backlog.settled());
    CHECK(started->is_ready());
    CHECK(started->value() == 42);
}

ASYNC_TEST("async backlog - a cold node is neither waited for nor started")
{
    cc::async_backlog backlog;
    auto const lazy = cc::make_async_lazy([] { return 1; });
    backlog.track(lazy);

    CHECK(backlog.outstanding_count() == 0);
    co_await cc::async_settled(backlog.settled());
    CHECK(lazy->is_cold()); // settling must not be what starts work nobody asked for
}

ASYNC_TEST("async backlog - an entry whose node is gone counts for nothing")
{
    cc::async_backlog backlog;
    {
        auto const abandoned = cc::make_async_manual<int>();
        backlog.track(abandoned);
        CHECK(backlog.outstanding_count() == 1);
    }

    // The weak entry is all that remains, so nothing is pinned and nothing parks.
    CHECK(backlog.outstanding_count() == 0);
    co_await cc::async_settled(backlog.settled());
}

// The rounds are the point of settled(): work a settling node tracks on its way out is still part of the backlog.
ASYNC_TEST("async backlog - settled waits for work tracked while it waits")
{
    cc::async_backlog backlog;
    auto const first = cc::make_async_manual<int>();
    auto const second = cc::make_async_manual<int>();
    backlog.track(first);

    auto const chain = backlog.start(
        [](cc::async_backlog& b, cc::shared_async<int> gate, cc::shared_async<int> follow_up) -> cc::shared_async<cc::unit>
        {
            co_await cc::async_settled(gate);
            b.track(follow_up); // before resolving, so any round that sees this node settled also sees what it tracked
            co_return;
        }(backlog, first, second));

    auto const settled = cc::async_start(backlog.settled());

    first->push_value(1);
    co_await cc::async_settled(chain);
    CHECK(!settled->is_ready()); // `second` is tracked and pending

    second->push_value(2);
    co_await cc::async_settled(settled);
    CHECK(backlog.outstanding_count() == 0);
}

ASYNC_TEST("async backlog - several backlogs settle together, across work one tracks into another")
{
    cc::async_backlog upstream;
    cc::async_backlog downstream;
    auto const gate = cc::make_async_manual<int>();
    auto const store = cc::make_async_manual<int>();

    // The shape of a compile whose cache store is queued before the compile resolves.
    auto const compile = upstream.start(
        [](cc::async_backlog& into, cc::shared_async<int> g,
           cc::shared_async<int> write_behind) -> cc::shared_async<cc::unit>
        {
            co_await cc::async_settled(g);
            into.track(write_behind);
            co_return;
        }(downstream, gate, store));

    cc::async_backlog const* const both[] = {&upstream, &downstream};
    auto const settled = cc::async_start(cc::async_backlog::settled(cc::span<cc::async_backlog const* const>(both)));

    gate->push_value(1);
    co_await cc::async_settled(compile);
    CHECK(!settled->is_ready()); // the store the compile queued belongs to the other backlog, and still counts

    store->push_value(2);
    co_await cc::async_settled(settled);
    CHECK(downstream.outstanding_count() == 0);
}

ASYNC_TEST("async backlog - settled may outlive the backlog it was taken from")
{
    auto const work = cc::make_async_manual<int>();
    auto settled = cc::shared_async<cc::unit>();
    {
        cc::async_backlog backlog;
        backlog.track(work);
        settled = cc::async_start(backlog.settled());
    }

    work->push_value(3);
    co_await cc::async_settled(settled);
    CHECK(settled->is_ready());
}

#if CC_HAS_THREADS
ASYNC_TEST("async backlog - work tracked from many pool nodes is all waited for")
{
    cc::async_backlog backlog;
    cc::atomic<i32> ran = {0};

    auto spawned = cc::vector<cc::shared_async<cc::unit>>();
    for (auto p = 0; p < 8; ++p)
        spawned.push_back(cc::async_start(
            [](cc::async_backlog& b, cc::atomic<i32>& counter) -> cc::shared_async<cc::unit>
            {
                for (auto i = 0; i < 50; ++i)
                    (void)b.start(cc::make_async_lazy(
                        [&counter]
                        {
                            counter.fetch_add(1);
                            return cc::unit{};
                        }));
                co_return;
            }(backlog, ran)));

    co_await cc::async_all(cc::span<cc::shared_async<cc::unit> const>(spawned));

    // Every start happened before this wait was taken, so all of it must have run by the time it resolves.
    co_await cc::async_settled(backlog.settled());
    CHECK(ran.load() == 400);
    CHECK(backlog.outstanding_count() == 0);
}
#endif
