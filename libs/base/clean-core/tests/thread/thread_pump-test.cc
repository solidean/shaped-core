#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

// The registry is what a threadless build's blocking waits rely on, so the invariants pinned here are the ones a
// deadlock would violate: a sweep reaches everybody, it never re-enters a pump, and a registration that dies stops
// being reachable before its owner does.
//
// Every test here is exclusive() and main_thread because the registry is PROCESS-GLOBAL.
// A sibling test blocking on an async sweeps it too, which would call these pumps at moments this file never chose —
// so running beside anything at all makes the observations here meaningless rather than merely flaky.
// exclusive() keeps the other tests out, and main_thread keeps out nexus's own main loop, which sweeps between bodies.

TEST("cc::thread_pump_all - a registration stops being reachable when it dies", main_thread, exclusive())
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

TEST("cc::thread_pump_all - one sweep reaches every registration", main_thread, exclusive())
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

TEST("cc::thread_pump_all - a pump reporting work makes the sweep report it", main_thread, exclusive())
{
    auto const busy = cc::register_thread_pump([] { return true; });

    CHECK(cc::thread_pump_all()); // "somebody progressed" is what keeps a driver from sleeping
}

TEST("cc::thread_pump_all - a pump is never re-entered", main_thread, exclusive())
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

TEST("cc::thread_pump_all - sweeping from inside a pump still reaches the others", main_thread, exclusive())
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

TEST("cc::thread_pump_registration - resetting stops the pump", main_thread, exclusive())
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

TEST("cc::thread_pump_registration - moving transfers the registration rather than copying it", main_thread, exclusive())
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

TEST("cc::thread_pump_all_for - a non-positive budget runs a single cycle", main_thread, exclusive())
{
    // A pump that never goes idle, so a budget that looped would not return at all: reaching the CHECK is the assertion.
    auto const busy = cc::register_thread_pump([] { return true; });

    CHECK(cc::thread_pump_all_for(0.0)); // one cycle, reporting work still pending
}

TEST("cc::thread_pump_all_for - returns false once everything goes idle", main_thread, exclusive())
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

TEST("cc::threaded_actor - an unthreaded actor is driven without being named", main_thread, exclusive())
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

TEST("cc::threaded_actor - an actor with a thread of its own registers nothing", main_thread, exclusive())
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

TEST("cc::threaded_actor - shutdown wakes a thread that is about to sleep", main_thread, exclusive())
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

TEST("cc::threaded_actor - shutdown deregisters, so a later sweep never touches the actor", main_thread, exclusive())
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

// Without threads a pool's drive is the only loop there is, so it sweeps the pump its graph waits on.
// It used to fall out as soon as its own queue and home were dry, reporting a graph no thread could complete while a
// registered pump held the one delivery it waited on.
#if !CC_HAS_THREADS
TEST("cc::thread_pump_all - without threads, a pool's drive sweeps the pump its graph waits on", main_thread, exclusive())
{
    cc::async_thread_pool pool(2);
    auto const delivered = cc::make_async_manual<int>();

    auto const pump = cc::register_thread_pump(
        [&]
        {
            if (delivered->is_ready())
                return false;
            delivered->push_value(41);
            return true;
        });

    auto const root = cc::make_async_lazy([](int v) { return v + 1; }, delivered);
    CHECK(cc::async_blocking_get_on(pool, root) == 42);
}
#endif

#if CC_HAS_THREADS
// With threads, a thread parked in a pool never runs a pump, however long it waits.
// An unthreaded component belongs to the loop that drives it; a parked pool thread running it would hand its handlers to
// whichever unrelated wait happened to be parked, racing that loop.
TEST("cc::thread_pump_all - a thread parked in a pool never runs a pump", main_thread, exclusive())
{
    cc::async_thread_pool pool(2);
    auto const delivered = cc::make_async_manual<int>();

    auto swept = cc::atomic<int>{0};
    auto const pump = cc::register_thread_pump(
        [&]
        {
            swept.fetch_add(1);
            return false;
        });

    // Delivered from a thread of its own, and announced, so a pool that swept on a pump signal would have its chance.
    auto poster = std::thread(
        [&]
        {
            cc::thread_pump_notify();
            delivered->push_value(41);
        });

    auto const root = cc::make_async_lazy([](int v) { return v + 1; }, delivered);
    CHECK(cc::async_blocking_get_on(pool, root) == 42);
    poster.join();
    CHECK(swept.load() == 0);
}

// A blocking drive on a scheduler without threads is the loop, so it drives the pump, and a post wakes it rather than a clock.
TEST("cc::threaded_actor - a post to an unthreaded actor wakes a blocking drive waiting on its reply",
     main_thread,
     exclusive())
{
    struct echo
    {
        int value = 0;
        cc::shared_async<int> reply;
    };
    struct echo_actor final : cc::threaded_actor_impl<echo>
    {
        void on_message(echo msg) override { static_cast<cc::async<int>&>(*msg.reply).push_value(msg.value + 1); }
    };

    auto actor = cc::make_threaded_actor<echo_actor>();
    actor->start(cc::threaded_actor_mode::unthreaded);

    auto const reply = cc::make_async_manual<int>();
    auto poster = std::thread([&] { (void)actor->enqueue_message(echo{.value = 41, .reply = reply}); });

    auto driver = cc::singlethreaded_scheduler();
    auto const scope = cc::async_worker_scope(driver);
    auto const root = cc::make_async_lazy([](int v) { return v; }, reply);
    CHECK(cc::async_blocking_get(root) == 42);
    poster.join();
    actor->shutdown();
}
#endif
