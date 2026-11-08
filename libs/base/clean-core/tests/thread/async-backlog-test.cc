#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh> // CC_DEFER
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

ASYNC_TEST("async backlog - an empty backlog settles at once")
{
    cc::async_backlog const backlog;
    co_await cc::async_settled(backlog.settled());
    CHECK(backlog.outstanding_count() == 0);
}

// Resolved rather than lazy, so a teardown with no scheduler left can still block on it.
TEST("async backlog - nothing pending hands back an already-resolved node")
{
    cc::async_backlog backlog;
    CHECK(backlog.settled()->is_ready());

    // Settled and cold entries are not pending either.
    auto const lazy = cc::make_async_lazy([] { return 1; });
    backlog.track(lazy);
    auto const done = cc::make_async_manual<int>();
    backlog.track(done);
    done->push_value(2);
    CHECK(backlog.settled()->is_ready());
}

// The first round is pinned at the call: work started afterwards is not part of it.
TEST("async backlog - work tracked after settled() was taken is not waited for")
{
    cc::async_backlog backlog;
    auto const settled = backlog.settled();

    auto const later = cc::make_async_manual<int>();
    backlog.track(later);

    CHECK(settled->is_ready());
    CHECK(!later->is_ready());
    later->push_value(0);
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

// A production backlog is never settled, so tracking alone must keep dead entries bounded, even behind a live one.
TEST("async backlog - dead entries behind a live one are reclaimed without settling")
{
    cc::async_backlog backlog;
    auto const held_cold = cc::make_async_lazy([] { return 0; });
    backlog.track(held_cold);

    for (auto i = 0; i < 1000; ++i)
    {
        auto const work = cc::make_async_manual<int>();
        backlog.track(work);
        work->push_value(i);
    }

    // One live entry, so compaction keeps the ring below its 32-entry floor rather than at a thousand.
    CHECK(backlog.tracked_count() <= 32);
    CHECK(held_cold->is_cold()); // compacting keeps a cold entry and must not be what starts it
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

// The registry, which is what lets a hang report say WHOSE work is outstanding rather than only that some is.
//
// It covers tracked work only -- a node awaited in the ordinary way is in nobody's list -- and the report says so
// rather than implying a completeness it does not have.

namespace
{
/// How many live backlogs carry `name`, and what the first of them still owes.
struct registry_probe
{
    isize found = 0;
    isize outstanding = 0;
    bool readable = false;
};

[[nodiscard]] registry_probe probe_registry(cc::string_view name)
{
    auto probe = registry_probe();
    probe.readable = cc::try_for_each_async_backlog(
        [&](cc::async_backlog const& b)
        {
            if (b.name() != name)
                return;
            if (probe.found == 0)
                probe.outstanding = b.outstanding_count();
            ++probe.found;
        });
    return probe;
}
} // namespace

TEST("async_backlog - a live backlog is findable by name, and a destroyed one is not")
{
    auto const name = cc::string_view("backlog-under-test");

    CHECK(probe_registry(name).found == 0);

    {
        auto const backlog = cc::async_backlog(name);
        CHECK(backlog.name() == name);

        auto const probe = probe_registry(name);
        CHECK(probe.readable);
        CHECK(probe.found == 1);
        CHECK(probe.outstanding == 0);
    }

    // Unregistered on destruction, because a report walking a dangling entry is worse than
    // one reporting nothing.
    CHECK(probe_registry(name).found == 0);
}

TEST("async_backlog - the registry reports what a backlog still owes")
{
    auto const name = cc::string_view("backlog-with-outstanding-work");
    auto backlog = cc::async_backlog(name);

    auto const work = cc::make_async_manual<int>();
    backlog.track(work);
    CHECK(backlog.outstanding_count() == 1);

    auto const during = probe_registry(name);
    CHECK(during.readable);
    REQUIRE(during.found == 1);
    CHECK(during.outstanding == 1);

    work->push_value(7);

    CHECK(probe_registry(name).outstanding == 0);
}

TEST("async_backlog - an unnamed backlog is still findable, just not attributable")
{
    auto const backlog = cc::async_backlog();
    CHECK(backlog.name().empty());

    auto seen = 0;
    CHECK(cc::try_for_each_async_backlog([&](cc::async_backlog const&) { ++seen; }));
    CHECK(seen > 0);
}

namespace
{
cc::shared_async<cc::unit> await_both(cc::shared_async<int> failing, cc::shared_async<int> pending)
{
    co_await cc::async_all(failing, pending);
}

cc::shared_async<cc::unit> await_span(cc::vector<cc::shared_async<int>> deps)
{
    co_await cc::async_all(cc::span<cc::shared_async<int> const>(deps));
}
} // namespace

// async_all returns on its first failed member; the others run on, awaited by nobody, unless something records them.
// A tracked node that is settled or cold counts for nothing, so only the pending member is owed.
// The abandoned work is process-wide, so each test here counts it alone.
ASYNC_TEST("async backlog - async_all that fails hands its members still running to the abandoned work", exclusive())
{
    auto const before = cc::async_abandoned_work().outstanding_count();
    auto const failing = cc::make_async_from_error<int>(cc::async_error::make_error(cc::any_error("boom")));
    auto const pending = cc::make_async_manual<int>();

    co_await cc::async_settled(cc::async_start(await_both(failing, pending)));
    CHECK(cc::async_abandoned_work().outstanding_count() == before + 1);

    auto const pending_too = cc::make_async_manual<int>();
    auto deps = cc::vector<cc::shared_async<int>>();
    deps.push_back(failing);
    deps.push_back(pending_too);
    co_await cc::async_settled(cc::async_start(await_span(cc::move(deps))));
    CHECK(cc::async_abandoned_work().outstanding_count() == before + 2);

    pending->push_value(1);
    pending_too->push_value(2);
    co_await cc::async_settled(cc::async_abandoned_work().settled());
    CHECK(cc::async_abandoned_work().outstanding_count() == before);
}

#if CC_HAS_THREADS
namespace
{
cc::shared_async<cc::unit> await_one(cc::shared_async<int> work)
{
    (void)(co_await work);
}
} // namespace

// The awaiter parks on work running elsewhere, and its last handle is gone, so it is torn down parked.
// The work runs on, and only the abandoned work still knows of it.
TEST("async backlog - a parked awaiter torn down hands the work it awaited, still running, to the abandoned work",
     nx::config::no_scheduler,
     exclusive())
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    auto const before = cc::async_abandoned_work().outstanding_count();
    auto entered = cc::atomic<bool>(false);
    auto release = cc::atomic<bool>(false);
    auto const work = cc::async_start(cc::make_async_lazy(
        [&]
        {
            entered.store(true);
            while (!release.load())
                cc::this_thread_yield();
            return 1;
        }));
    while (!entered.load())
        cc::this_thread_yield();

    (void)cc::async_start(await_one(work));
    // bounded only so that a missing hook fails instead of hanging
    auto const t0 = cc::current_time_steady_secs();
    while (cc::async_abandoned_work().outstanding_count() == before && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    CHECK(cc::async_abandoned_work().outstanding_count() == before + 1);

    release.store(true);
    auto const t1 = cc::current_time_steady_secs();
    while (cc::async_abandoned_work().outstanding_count() > before && cc::current_time_steady_secs() - t1 < 10.0)
        cc::this_thread_yield();
    CHECK(cc::async_abandoned_work().outstanding_count() == before);
}

namespace
{
/// A capture whose destruction holds until `release`, and says where it is: 1 begun, 2 ended.
struct slow_capture
{
    cc::atomic<int>* stage = nullptr;
    cc::atomic<bool>* release = nullptr;

    slow_capture(cc::atomic<int>* s, cc::atomic<bool>* r) : stage(s), release(r) {}
    slow_capture(slow_capture&& rhs) noexcept
      : stage(cc::exchange(rhs.stage, nullptr)), release(cc::exchange(rhs.release, nullptr))
    {
    }
    slow_capture& operator=(slow_capture&&) = delete;
    ~slow_capture()
    {
        if (stage == nullptr)
            return;
        stage->store(1);
        // bounded only so that a test that never releases fails instead of hanging
        auto const t0 = cc::current_time_steady_secs();
        while (!release->load() && cc::current_time_steady_secs() - t0 < 10.0)
            cc::this_thread_yield();
        stage->store(2);
    }
};
} // namespace

// Its last handle gone, the node reads as gone to a weak lock, but its teardown still runs on the thread that dropped it.
// What that teardown releases, captures and context, is part of the work: settled must not return before it ends.
TEST("async backlog - settled waits for a node dropped unresolved to finish its teardown",
     nx::config::no_scheduler,
     exclusive())
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    auto stage = cc::atomic<int>(0);
    auto release = cc::atomic<bool>(false);
    auto backlog = cc::async_backlog("teardown");
    auto node = cc::make_async_lazy([held = slow_capture(&stage, &release)] { return 0; });
    backlog.track(node);

    auto dropper = cc::native_thread::start([n = cc::move(node)]() mutable { n = {}; });
    // however the test ends, so a failed REQUIRE does not leave the thread unjoined
    CC_DEFER
    {
        release.store(true);
        dropper.join();
    };
    // bounded only so that a dropper that never runs fails instead of hanging
    auto const t0 = cc::current_time_steady_secs();
    while (stage.load() == 0 && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    REQUIRE(stage.load() == 1);
    CHECK(backlog.outstanding_count() == 1); // no strong handle is left, and it still counts

    auto const settled = cc::async_start(backlog.settled());
    CHECK(!settled->is_ready()); // the capture's destructor holds, so the teardown is still running here
    release.store(true);
    auto const t1 = cc::current_time_steady_secs();
    while (!settled->is_ready() && cc::current_time_steady_secs() - t1 < 10.0)
        cc::this_thread_yield();
    CHECK(settled->is_ready());
    CHECK(stage.load() == 2);
    CHECK(backlog.outstanding_count() == 0);
}

namespace
{
/// Clears `alive` when the frame it lives in is destroyed, so a test can see when that happens.
struct frame_probe
{
    cc::atomic<bool>* alive = nullptr;

    explicit frame_probe(cc::atomic<bool>* a) : alive(a) {}
    frame_probe(frame_probe const&) = delete;
    frame_probe& operator=(frame_probe const&) = delete;
    ~frame_probe() { alive->store(false); }
};

/// What a fan-out's children share, owned by them rather than by the frame that started them.
struct fan_out_state
{
    cc::atomic<int> entered = {0};
    cc::atomic<bool> release = {false};
    cc::atomic<int> finished = {0};
};

cc::shared_async<cc::unit> await_all_of(cc::span<cc::shared_async<cc::unit> const> work)
{
    co_await cc::async_all(work);
}

/// A fan-out two levels below a frame: this frame awaits a coroutine that awaits the children.
/// Each child holds the state it writes, so it is safe whatever becomes of this frame.
cc::shared_async<cc::unit> fan_out_from_frame(cc::atomic<bool>* alive, cc::shared_ptr<fan_out_state> state)
{
    auto const probe = frame_probe(alive);
    auto children = cc::vector<cc::shared_async<cc::unit>>();
    for (auto i = 0; i < 2; ++i)
        children.push_back(cc::async_start(cc::make_async_lazy(
            [state]
            {
                state->entered.fetch_add(1);
                // bounded only so that a test that never releases fails instead of hanging
                auto const t0 = cc::current_time_steady_secs();
                while (!state->release.load() && cc::current_time_steady_secs() - t0 < 10.0)
                    cc::this_thread_yield();
                state->finished.fetch_add(1);
                return cc::unit{};
            })));
    // running on the pool before they are awaited, so none is driven inline here and the frames above them park
    auto const t0 = cc::current_time_steady_secs();
    while (state->entered.load() < 2 && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    co_await await_all_of(cc::span<cc::shared_async<cc::unit> const>(children));
}

/// Awaits `work`, holding `keep` until it resolves: it reads `work`, which may live in the frame `keep` holds.
cc::shared_async<cc::unit> await_all_holding(cc::span<cc::shared_async<cc::unit> const> work, cc::async_node_ptr keep)
{
    co_await cc::async_all(work);
    (void)keep;
}

/// The same fan-out, its children reaching into this frame, and holding this coroutine's own node so that they may.
/// The coroutine awaiting them reads this frame's `children` after them, so it holds the node as well.
cc::shared_async<cc::unit> fan_out_holding_frame(cc::atomic<bool>* alive,
                                                 cc::shared_ptr<fan_out_state> state,
                                                 cc::atomic<int>* saw_frame)
{
    auto const probe = frame_probe(alive);
    auto self = co_await cc::async_this_node();
    auto children = cc::vector<cc::shared_async<cc::unit>>();
    for (auto i = 0; i < 2; ++i)
        children.push_back(cc::async_start(cc::make_async_lazy(
            [state, self, alive, saw_frame]
            {
                state->entered.fetch_add(1);
                // bounded only so that a test that never releases fails instead of hanging
                auto const t0 = cc::current_time_steady_secs();
                while (!state->release.load() && cc::current_time_steady_secs() - t0 < 10.0)
                    cc::this_thread_yield();
                if (alive->load())
                    saw_frame->fetch_add(1);
                state->finished.fetch_add(1);
                return cc::unit{};
            })));
    auto const t0 = cc::current_time_steady_secs();
    while (state->entered.load() < 2 && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    // moved into the awaiting coroutine, so this frame keeps none of itself across the suspension
    auto all = await_all_holding(cc::span<cc::shared_async<cc::unit> const>(children), cc::move(self));
    co_await all;
}

cc::shared_async<cc::unit> await_forever(cc::atomic<bool>* alive, cc::atomic<bool>* entered, cc::shared_async<int> never)
{
    auto const probe = frame_probe(alive);
    entered->store(true);
    (void)(co_await never);
}
} // namespace

// The contract a fan-out must be written to: a parked frame dropped unresolved is destroyed at once, and the work below
// it runs on, abandoned, so that work must own everything it touches and never reach into the frame.
// Pinned 2026-11-08, after a consumer's parallel helper whose children wrote through references to its frame's locals
// was found under ASan as a double free; see "Work that outlives its frame" in libs/base/clean-core/docs/systems/async.md.
TEST("async backlog - a parked frame dropped while work below it runs is destroyed at once, so that work must own its "
     "state",
     nx::config::no_scheduler,
     exclusive())
{
    cc::async_thread_pool pool(3);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    auto const before = cc::async_abandoned_work().outstanding_count();
    auto alive = cc::atomic<bool>(true);
    auto const state = cc::make_shared<fan_out_state>();
    // the parent is a scheduled node's argument, so its last handle goes when that node is dropped unresolved
    auto outer = cc::make_async_scheduled([](cc::unit) { return 1; }, fan_out_from_frame(&alive, state));
    CC_DEFER
    {
        state->release.store(true);
    };
    auto const t0 = cc::current_time_steady_secs();
    while (state->entered.load() < 2 && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    REQUIRE(state->entered.load() == 2);

    // the frame goes with the drop, once the poller lets go, while both children still run
    outer = {};
    auto const t1 = cc::current_time_steady_secs();
    while (alive.load() && cc::current_time_steady_secs() - t1 < 10.0)
        cc::this_thread_yield();
    CHECK(!alive.load());
    CHECK(state->finished.load() == 0);

    // they finish on what they own, and only the abandoned work still knew of them
    state->release.store(true);
    auto const t2 = cc::current_time_steady_secs();
    while ((state->finished.load() < 2 || cc::async_abandoned_work().outstanding_count() > before)
           && cc::current_time_steady_secs() - t2 < 10.0)
        cc::this_thread_yield();
    CHECK(state->finished.load() == 2);
    CHECK(cc::async_abandoned_work().outstanding_count() == before);
}

// Work that holds the coroutine's own node keeps its frame for as long as it runs, so it may reach into it.
// Once that work is done the frame goes, a drop still a drop: the coroutine runs no further than it had.
TEST("async backlog - children holding async_this_node keep their dropped frame alive while they run, then let it go",
     nx::config::no_scheduler,
     exclusive())
{
    cc::async_thread_pool pool(3);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    auto const before = cc::async_abandoned_work().outstanding_count();
    auto alive = cc::atomic<bool>(true);
    auto saw_frame = cc::atomic<int>(0);
    auto const state = cc::make_shared<fan_out_state>();
    auto outer = cc::make_async_scheduled([](cc::unit) { return 1; }, fan_out_holding_frame(&alive, state, &saw_frame));
    CC_DEFER
    {
        state->release.store(true);
    };
    auto const t0 = cc::current_time_steady_secs();
    while (state->entered.load() < 2 && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    REQUIRE(state->entered.load() == 2);

    // dropped while they run: a premature teardown has time to show, and does not happen
    outer = {};
    auto const dropped = cc::current_time_steady_secs();
    while (alive.load() && cc::current_time_steady_secs() - dropped < 0.2)
        cc::this_thread_yield();
    CHECK(alive.load());

    state->release.store(true);
    auto const t1 = cc::current_time_steady_secs();
    while (alive.load() && cc::current_time_steady_secs() - t1 < 10.0)
        cc::this_thread_yield();
    CHECK(!alive.load());
    CHECK(saw_frame.load() == 2);
    auto const t2 = cc::current_time_steady_secs();
    while (cc::async_abandoned_work().outstanding_count() > before && cc::current_time_steady_secs() - t2 < 10.0)
        cc::this_thread_yield();
    CHECK(cc::async_abandoned_work().outstanding_count() == before);
}

// What it awaits runs nowhere, and its frame is torn down at once.
TEST("async backlog - a parked frame whose work runs nowhere is torn down at once", nx::config::no_scheduler, exclusive())
{
    cc::async_thread_pool pool(2);
    cc::scoped_compute_async_scheduler const as_compute(pool);

    auto alive = cc::atomic<bool>(true);
    auto entered = cc::atomic<bool>(false);
    auto const never = cc::make_async_manual<int>();
    auto parent = cc::async_start(await_forever(&alive, &entered, never));
    auto const t0 = cc::current_time_steady_secs();
    while (!entered.load() && cc::current_time_steady_secs() - t0 < 10.0)
        cc::this_thread_yield();
    REQUIRE(entered.load());

    // dropped parked, or dropped running and parked a moment later: either way torn down, without the manual node resolving
    parent = {};
    auto const t1 = cc::current_time_steady_secs();
    while (alive.load() && cc::current_time_steady_secs() - t1 < 10.0)
        cc::this_thread_yield();
    CHECK(!alive.load());
    never->push_value(1);
}
#endif
