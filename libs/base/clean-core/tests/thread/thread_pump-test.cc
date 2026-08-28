#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <nexus/test.hh>

// The registry is what a threadless build's blocking waits rely on, so the invariants pinned here are the ones a
// deadlock would violate: a sweep reaches everybody, it never re-enters a pump, and a registration that dies stops
// being reachable before its owner does.
//
// Every test here is exclusive() because the registry is PROCESS-GLOBAL.
// A sibling test blocking on an async sweeps it too, which would call these pumps at moments this file never chose —
// so running beside anything at all makes the observations here meaningless rather than merely flaky.

TEST("cc::thread_pump_all - a registration stops being reachable when it dies", exclusive())
{
    auto ran = false;
    {
        auto const registration = cc::register_thread_pump(
            [&]
            {
                ran = true;
                return false;
            });
        CHECK(registration.is_registered());
    }

    (void)cc::thread_pump_all();
    CHECK(!ran); // the registration died with its scope, before `ran` could
}

TEST("cc::thread_pump_all - one sweep reaches every registration", exclusive())
{
    auto first_ran = false;
    auto second_ran = false;

    auto const first = cc::register_thread_pump(
        [&]
        {
            first_ran = true;
            return false;
        });
    auto const second = cc::register_thread_pump(
        [&]
        {
            second_ran = true;
            return false;
        });

    (void)cc::thread_pump_all();
    CHECK(first_ran);
    CHECK(second_ran); // a sweep does not stop at the first pump, however that one answered
}

TEST("cc::thread_pump_all - a pump reporting work makes the sweep report it", exclusive())
{
    auto const busy = cc::register_thread_pump([] { return true; });

    CHECK(cc::thread_pump_all()); // "somebody progressed" is what keeps a driver from sleeping
}

TEST("cc::thread_pump_all - a pump is never re-entered", exclusive())
{
    // The case the whole guard exists for: a handler sweeps — which is how it waits for a sibling — and that sweep must
    // not dispatch this same pump on top of itself, exactly as a busy thread takes no new work.
    auto depth = 0;
    auto max_depth = 0;
    auto swept_from_inside = false;

    auto const self = cc::register_thread_pump(
        [&]
        {
            ++depth;
            max_depth = cc::max(max_depth, depth);
            if (!swept_from_inside)
            {
                swept_from_inside = true;
                (void)cc::thread_pump_all();
            }
            --depth;
            return false;
        });

    (void)cc::thread_pump_all();
    CHECK(swept_from_inside);
    CHECK(max_depth == 1);
}

TEST("cc::thread_pump_all - sweeping from inside a pump still reaches the others", exclusive())
{
    // The other half of the guard: skipping the RUNNING pump must not skip the rest, or a handler waiting on a sibling
    // actor would wait forever — which is the deadlock the registry exists to prevent.
    auto sibling_ran = false;
    auto const sibling = cc::register_thread_pump(
        [&]
        {
            sibling_ran = true;
            return false;
        });

    auto swept = false;
    auto const self = cc::register_thread_pump(
        [&]
        {
            if (!swept)
            {
                swept = true;
                (void)cc::thread_pump_all();
                CHECK(sibling_ran); // resolved by OUR nested sweep, before this pump returned
            }
            return false;
        });

    (void)cc::thread_pump_all();
    CHECK(swept);
}

TEST("cc::thread_pump_registration - resetting stops the pump", exclusive())
{
    auto ran = false;
    auto registration = cc::register_thread_pump(
        [&]
        {
            ran = true;
            return false;
        });

    registration.reset();
    CHECK(!registration.is_registered());

    ran = false;
    (void)cc::thread_pump_all();
    CHECK(!ran);
}

TEST("cc::thread_pump_registration - moving transfers the registration rather than copying it", exclusive())
{
    auto ran = false;
    auto first = cc::register_thread_pump(
        [&]
        {
            ran = true;
            return false;
        });

    auto second = cc::move(first);
    CHECK(!first.is_registered()); // the moved-from handle deregisters nothing when it dies
    CHECK(second.is_registered());

    (void)cc::thread_pump_all();
    CHECK(ran);

    second.reset();
    ran = false;
    (void)cc::thread_pump_all();
    CHECK(!ran); // one handle owned it, so one reset is enough
}

TEST("cc::thread_pump_all_for - a non-positive budget runs a single cycle", exclusive())
{
    // A pump that never goes idle, so a budget that looped would not return at all: reaching the CHECK is the assertion.
    auto const busy = cc::register_thread_pump([] { return true; });

    CHECK(cc::thread_pump_all_for(0.0)); // one cycle, reporting work still pending
}

TEST("cc::thread_pump_all_for - returns false once everything goes idle", exclusive())
{
    auto remaining = 3;
    auto const draining = cc::register_thread_pump([&] { return remaining-- > 0; });

    (void)cc::thread_pump_all_for(1000.0);
    CHECK(remaining < 0); // swept until it stopped reporting work, rather than stopping on the budget
}

namespace
{
struct counting_actor : cc::threaded_actor_impl<int>
{
    cc::vector<int> seen;

    void on_message(int value) override { seen.push_back(value); }
};
} // namespace

TEST("cc::threaded_actor - an unthreaded actor is driven without being named", exclusive())
{
    // The whole point of the registry: nothing here mentions the actor, and its message still gets dispatched.
    auto const baseline = cc::registered_thread_pump_count();
    auto actor = cc::make_threaded_actor<counting_actor>();
    actor->start(cc::threaded_actor_mode::unthreaded);
    CHECK(cc::registered_thread_pump_count() == baseline + 1);

    REQUIRE(actor->enqueue_message(7));
    (void)cc::thread_pump_all();
    actor->shutdown();
    CHECK(cc::registered_thread_pump_count() == baseline); // what nexus's end-of-run leak check reads

    auto const impl = actor->take_impl<counting_actor>();
    REQUIRE(impl->seen.size() == 1);
    CHECK(impl->seen[0] == 7);
}

TEST("cc::threaded_actor - an actor with a thread of its own registers nothing", exclusive())
{
    auto const baseline = cc::registered_thread_pump_count();
    auto actor = cc::make_and_start_threaded_actor<counting_actor>();
    CHECK(cc::registered_thread_pump_count() == baseline + (CC_HAS_THREADS ? 0 : 1));
    actor->shutdown();
    CHECK(cc::registered_thread_pump_count() == baseline);
}

namespace
{
/// Announces that the actor thread is running, so a test can aim at what the thread does next.
cc::atomic<bool> g_actor_thread_started = false;

struct signaling_actor : cc::threaded_actor_impl<int>
{
    void on_thread_init() override { g_actor_thread_started.store(true); }
    void on_message(int) override {}
};
} // namespace

TEST("cc::threaded_actor - shutdown wakes a thread that is about to sleep", exclusive())
{
    // The race: shutdown() flips a flag the actor thread's wait predicate reads, and that flag is not the inbox mutex's.
    // A notify sent while the thread sits between its last predicate check and the wait it is about to enter is lost,
    // and the join below then never returns.
    // That window is only a few instructions wide, so waiting for the thread to announce itself is what makes aiming at
    // it possible at all — hammering start/shutdown blind just races thread startup, which is a thousand times longer.
    // The spin then sweeps the shutdown across the handful of steps between the announcement and the wait.
    auto volatile sink = 0;
    for (auto i = 0; i < 400; ++i)
    {
        g_actor_thread_started.store(false);
        auto actor = cc::make_and_start_threaded_actor<signaling_actor>();

        while (!g_actor_thread_started.load())
            sink = sink + 1;
        for (auto s = 0; s < i; ++s)
            sink = sink + 1;

        actor->shutdown();
    }
    CHECK(true); // reaching here at all is the assertion; a lost wakeup hangs rather than fails
}

TEST("cc::threaded_actor - shutdown deregisters, so a later sweep never touches the actor", exclusive())
{
    // The lifetime half: a pump outliving its actor is a sweep into freed memory, and nothing else would catch it.
    auto const baseline = cc::registered_thread_pump_count();
    auto actor = cc::make_threaded_actor<counting_actor>();
    actor->start(cc::threaded_actor_mode::unthreaded);
    actor->shutdown();
    CHECK(cc::registered_thread_pump_count() == baseline);

    REQUIRE(!actor->enqueue_message(1)); // shut down: the message is refused rather than queued
    (void)cc::thread_pump_all();

    auto const impl = actor->take_impl<counting_actor>();
    CHECK(impl->seen.empty());
}
