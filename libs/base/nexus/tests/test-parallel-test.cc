#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/spin.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <thread>

using namespace cc::primitive_defines;

// What --jobs must preserve, and what it must actually deliver.
//
// Every test here nests a run, so every test here is no_scheduler — a run stands up its own scheduler and schedulers do not nest.
// The inner runs are the ones under test, and they are the ones carrying `jobs`.

namespace
{
nx::test_schedule_config with_jobs(int jobs)
{
    nx::test_schedule_config config;
    config.jobs = jobs;
    return config;
}
} // namespace

TEST("parallel - a -jN run executes every test, and reports in schedule order", no_scheduler)
{
    nx::test_registry reg;
    for (auto i = 0; i < 16; ++i)
        reg.add_declaration(cc::format("t{:02}", i), {}, [] { CHECK(true); });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    REQUIRE(exec.executions.size() == 16);
    CHECK(exec.count_total_tests() == 16);
    CHECK(exec.count_total_checks() == 16);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.orphan_checks == 0);

    // Results are written into pre-sized slots by index, so the report order is the schedule's however the tests ran.
    for (auto i = 0; i < 16; ++i)
        CHECK(exec.executions[i].instance.declaration->name == cc::format("t{:02}", i));
}

TEST("parallel - a check is billed to its own test, whichever worker ran it", no_scheduler)
{
    // Each test contributes a distinct number of checks, so a misattribution shows up as a wrong per-test count rather than a right total.
    nx::test_registry reg;
    for (auto i = 1; i <= 8; ++i)
        reg.add_declaration(cc::format("t{}", i), {},
                            [i]
                            {
                                for (auto k = 0; k < i; ++k)
                                    CHECK(true);
                            });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    REQUIRE(exec.executions.size() == 8);
    for (auto i = 1; i <= 8; ++i)
        CHECK(exec.executions[i - 1].root.executed_checks == i);
}

TEST("parallel - a failing test fails alone, and never poisons the tests behind it", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("ok_before", {}, [] { CHECK(true); });
    reg.add_declaration("boom", {}, [] { CHECK(false); });
    reg.add_declaration("ok_after", {}, [] { CHECK(true); });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    // A test node always resolves to a VALUE — a failure is data on the execution, never an async error.
    // On the error channel it would propagate into the phase's join.
    REQUIRE(exec.executions.size() == 3);
    CHECK(!exec.executions[0].is_considered_failing());
    CHECK(exec.executions[1].is_considered_failing());
    CHECK(!exec.executions[2].is_considered_failing());
    CHECK(exec.count_failed_tests() == 1);
}

TEST("parallel - -j1 runs the tests in schedule order", no_scheduler)
{
    // The order property -j1 owes today's behavior: not "some order the graph settled on", but the schedule's.
    cc::vector<cc::string> order;
    nx::test_registry reg;
    for (auto i = 0; i < 8; ++i)
        reg.add_declaration(cc::format("t{}", i), {},
                            [&order, i]
                            {
                                order.push_back(cc::format("t{}", i));
                                CHECK(true);
                            });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(1));

    CHECK(exec.count_failed_tests() == 0);
    REQUIRE(order.size() == 8);
    for (auto i = 0; i < 8; ++i)
        CHECK(order[i] == cc::format("t{}", i));
}

TEST("parallel - exclusive tag holders never overlap", no_scheduler)
{
    // "gpu" holders must be serialized against each other while the untagged tests are free to run alongside them.
    cc::atomic<int> gpu_live = {0};
    cc::atomic<int> gpu_overlaps = {0};
    cc::vector<cc::string> gpu_order;
    cc::mutex<int> gpu_order_lock;

    nx::test_registry reg;
    for (auto i = 0; i < 6; ++i)
    {
        auto const gpu_body = [&, i]
        {
            if (gpu_live.fetch_add(1, cc::memory_order_acq_rel) != 0)
                gpu_overlaps.fetch_add(1, cc::memory_order_relaxed);
            gpu_order_lock.lock([&](int&) { gpu_order.push_back(cc::format("g{}", i)); });
            for (auto spin = 0; spin < 20000; ++spin)
                cc::spin_pause();
            gpu_live.fetch_sub(1, cc::memory_order_acq_rel);
            CHECK(true);
        };
        reg.add_declaration(cc::format("g{}", i), nx::impl::merge_config(nx::config::exclusive("gpu")), gpu_body);
        reg.add_declaration(cc::format("free{}", i), {}, [] { CHECK(true); });
    }

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(gpu_overlaps.load(cc::memory_order_acquire) == 0);

    // A lock serializes the holders without fixing their order under -jN: every holder ran, each exactly once.
    REQUIRE(gpu_order.size() == 6);
    for (auto i = 0; i < 6; ++i)
    {
        auto count = 0;
        for (auto const& g : gpu_order)
            count += g == cc::format("g{}", i) ? 1 : 0;
        CHECK(count == 1);
    }
}

TEST("parallel - a no-arg exclusive test runs alone", no_scheduler)
{
    cc::atomic<int> live = {0};
    cc::atomic<int> seen_beside_the_exclusive = {0};
    cc::atomic<bool> exclusive_running = {false};

    nx::test_registry reg;
    auto const busy = [&]
    {
        live.fetch_add(1, cc::memory_order_acq_rel);
        if (exclusive_running.load(cc::memory_order_acquire))
            seen_beside_the_exclusive.fetch_add(1, cc::memory_order_relaxed);
        for (auto spin = 0; spin < 20000; ++spin)
            cc::spin_pause();
        live.fetch_sub(1, cc::memory_order_acq_rel);
        CHECK(true);
    };

    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("before{}", i), {}, busy);
    reg.add_declaration("alone", nx::impl::merge_config(nx::config::exclusive()),
                        [&]
                        {
                            exclusive_running.store(true, cc::memory_order_release);
                            CHECK(live.load(cc::memory_order_acquire) == 0); // nothing before it may still be running
                            for (auto spin = 0; spin < 20000; ++spin)
                                cc::spin_pause();
                            exclusive_running.store(false, cc::memory_order_release);
                        });
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("after{}", i), {}, busy);

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(seen_beside_the_exclusive.load(cc::memory_order_acquire) == 0);
}

// Interleaved in schedule order, each exclusive() would split the phase into waves no longer than its slowest test.
TEST("parallel - exclusive tests run after every other test of their phase, wherever they are declared", no_scheduler)
{
    for (auto const jobs : {1, 4})
    {
        cc::atomic<int> finished_shared = {0};
        cc::atomic<int> exclusive_early = {0};

        nx::test_registry reg;
        auto const exclusive_body = [&]
        {
            if (finished_shared.load(cc::memory_order_acquire) != 6)
                exclusive_early.fetch_add(1, cc::memory_order_relaxed);
            CHECK(true);
        };
        auto const shared_body = [&]
        {
            finished_shared.fetch_add(1, cc::memory_order_acq_rel);
            CHECK(true);
        };

        reg.add_declaration("alone-first", nx::impl::merge_config(nx::config::exclusive()), exclusive_body);
        for (auto i = 0; i < 3; ++i)
            reg.add_declaration(cc::format("shared{}", i), {}, shared_body);
        reg.add_declaration("alone-middle", nx::impl::merge_config(nx::config::exclusive()), exclusive_body);
        for (auto i = 3; i < 6; ++i)
            reg.add_declaration(cc::format("shared{}", i), nx::impl::merge_config(nx::config::exclusive("tag")),
                                shared_body);

        auto const schedule = nx::test_schedule::create({}, reg);
        auto const exec = nx::execute_tests(schedule, with_jobs(jobs));

        CHECK(exec.count_failed_tests() == 0);
        CHECK(exclusive_early.load(cc::memory_order_acquire) == 0).context(cc::format("-j{}", jobs));
    }
}

// An exclusive ASYNC_TEST holds the phase lock across its suspends, and still has a scheduler to drive the root it hands back.
ASYNC_TEST("parallel - an exclusive ASYNC_TEST still gets a scheduler", exclusive())
{
    CHECK(true);
    co_return;
}

// nx::main_thread — a flag rather than a fourth scheduler mode, so it composes with the modes instead of excluding them.
// The outer body here runs on the run's own calling thread, which nx::run claimed as main, so a nested run can honour the flag.

TEST("parallel - a main_thread test runs on the process main thread", no_scheduler)
{
    // Compares against cc::thread_id::main, not merely "whoever called".
    REQUIRE(cc::current_thread_id() == cc::thread_id::main);

    auto pinned_thread = cc::thread_id::invalid;

    nx::test_registry reg;
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("busy{}", i), {}, [] { CHECK(true); });
    reg.add_declaration("pinned", nx::impl::merge_config(nx::config::main_thread),
                        [&]
                        {
                            pinned_thread = cc::current_thread_id();
                            CHECK(true);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(pinned_thread == cc::thread_id::main);
}

TEST("parallel - main_thread and no_scheduler are separate phases", no_scheduler)
{
    // A main_thread test is a node in the shared phase, and a no_scheduler one is driven in a phase of its own, so the two cannot be interleaved.
    // Phase order is first appearance, so every main_thread body runs before the first no_scheduler one; the no_scheduler phase keeps schedule order.
    cc::vector<cc::string> order;

    nx::test_registry reg;
    for (auto i = 0; i < 3; ++i)
    {
        reg.add_declaration(cc::format("m{}", i), nx::impl::merge_config(nx::config::main_thread),
                            [&order, i]
                            {
                                order.push_back(cc::format("m{}", i));
                                CHECK(true);
                            });
        reg.add_declaration(cc::format("n{}", i), nx::impl::merge_config(nx::config::no_scheduler),
                            [&order, i]
                            {
                                order.push_back(cc::format("n{}", i));
                                CHECK(true);
                            });
    }

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    REQUIRE(order.size() == 6);
    for (auto i = 0; i < 3; ++i)
    {
        CHECK(order[i].starts_with("m"));
        CHECK(order[3 + i] == cc::format("n{}", i));
    }
}

#if CC_HAS_THREADS
TEST("parallel - a main_thread test runs beside the shared phase", no_scheduler)
{
    // The overlap is forced rather than hoped for: each side waits, with a deadline, for the other to be live.
    REQUIRE(cc::current_thread_id() == cc::thread_id::main);
    cc::atomic<bool> main_live = {false};
    cc::atomic<bool> pool_live = {false};
    cc::atomic<bool> main_saw_pool = {false};
    cc::atomic<bool> pool_saw_main = {false};

    auto const wait_for = [](cc::atomic<bool>& flag)
    {
        auto const deadline = cc::current_time_steady_secs() + 10.0;
        while (!flag.load(cc::memory_order_acquire) && cc::current_time_steady_secs() < deadline)
            cc::this_thread_yield();
        return flag.load(cc::memory_order_acquire);
    };

    nx::test_registry reg;
    reg.add_declaration("on-main", nx::impl::merge_config(nx::config::main_thread),
                        [&]
                        {
                            main_live.store(true, cc::memory_order_release);
                            main_saw_pool.store(wait_for(pool_live), cc::memory_order_release);
                            CHECK(cc::current_thread_id() == cc::thread_id::main);
                        });
    reg.add_declaration("on-pool", {},
                        [&]
                        {
                            pool_live.store(true, cc::memory_order_release);
                            pool_saw_main.store(wait_for(main_live), cc::memory_order_release);
                            CHECK(true);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(main_saw_pool.load(cc::memory_order_acquire));
    CHECK(pool_saw_main.load(cc::memory_order_acquire));
}
#endif

TEST("parallel - main_thread composes with exclusive()", no_scheduler)
{
    // The body is handed to main, and the phase's exclusive lock keeps everything else out while it runs.
    auto pinned_thread = cc::thread_id::invalid;
    cc::atomic<int> live = {0};
    cc::atomic<int> seen_beside = {0};

    nx::test_registry reg;
    auto const busy = [&]
    {
        live.fetch_add(1, cc::memory_order_acq_rel);
        for (auto spin = 0; spin < 20000; ++spin)
            cc::spin_pause();
        live.fetch_sub(1, cc::memory_order_acq_rel);
        CHECK(true);
    };
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("busy{}", i), {}, busy);
    reg.add_declaration("pinned", nx::impl::merge_config(nx::config::main_thread, nx::config::exclusive()),
                        [&]
                        {
                            pinned_thread = cc::current_thread_id();
                            seen_beside.store(live.load(cc::memory_order_acquire), cc::memory_order_relaxed);
                            CHECK(true);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(pinned_thread == cc::thread_id::main);
    CHECK(seen_beside.load(cc::memory_order_acquire) == 0);
}

TEST("parallel - main_thread and own_pool cannot be combined", no_scheduler)
{
    // A private pool's worker is never the main thread, so honouring both is impossible and demoting one silently is the failure mode the flag exists to avoid.
    nx::test_registry reg;
    reg.add_declaration("pinned", nx::impl::merge_config(nx::config::main_thread, nx::config::own_pool(2)),
                        [] { CHECK(true); });

    auto const schedule = nx::test_schedule::create({}, reg);
    CHECK_ASSERTS(nx::execute_tests(schedule, with_jobs(2)));
}

#if CC_HAS_THREADS
namespace
{
/// Runs a test that fails while a partner is provably running, and returns whether the failure named that partner.
bool failure_names_its_partner()
{
    // A -jN failure is usually about what it ran BESIDE, and the report otherwise names only the test that failed.
    // The overlap is forced rather than hoped for: the failing test waits for a partner to be LIVE, and the partner stays live until it has failed.
    //
    // `live` counts tests currently inside their body rather than tests entered, which is what makes it an overlap detector:
    // a run that executed them back to back leaves it at 1, so the wait below runs out instead of reporting an overlap that never happened.
    // Both waits are bounded in wall-clock rather than in spins, because a spin budget is a bet on how fast the machine is —
    // 2M pauses is tens of milliseconds, less than a loaded runner spends handing the second test to a worker.
    cc::atomic<int> live = {0};
    cc::atomic<bool> has_failed = {false};
    cc::atomic<bool> overlapped = {false};

    // The bound is a deadlock guard rather than a budget: it is hundreds of times what the hand-off costs, so only a broken
    // one reaches it, and a broken one reports instead of hanging the suite.
    auto const wait_until = [](auto&& done)
    {
        auto const deadline = cc::current_time_steady_secs() + 5.0;
        while (!done() && cc::current_time_steady_secs() < deadline)
            cc::spin_pause();
        return done();
    };

    nx::test_registry reg;
    reg.add_declaration("partner", {},
                        [&]
                        {
                            live.fetch_add(1, cc::memory_order_acq_rel);
                            wait_until([&] { return has_failed.load(cc::memory_order_acquire); });
                            CHECK(true);
                            live.fetch_sub(1, cc::memory_order_acq_rel);
                        });
    reg.add_declaration("failing", {},
                        [&]
                        {
                            live.fetch_add(1, cc::memory_order_acq_rel);
                            auto const beside = wait_until([&] { return live.load(cc::memory_order_acquire) >= 2; });
                            overlapped.store(beside, cc::memory_order_release);
                            CHECK(false); // the failure under test
                            has_failed.store(true, cc::memory_order_release);
                            live.fetch_sub(1, cc::memory_order_acq_rel);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    // Required rather than hoped for: `with_jobs(4)` stands up three workers besides the thread driving the run, so a body
    // parked in its wait leaves the other test to one of them whatever the machine's core count is.
    // A timeout here is therefore a defect in that hand-off, and the message check below would have nothing to check.
    REQUIRE(overlapped.load(cc::memory_order_acquire));

    REQUIRE(exec.executions.size() == 2);
    auto const& failing = exec.executions[1];
    REQUIRE(failing.root.errors.size() == 1);

    auto beside = false;
    for (auto const& line : failing.root.errors[0].extra_lines)
        if (line.contains("partner"))
            beside = true;
    return beside;
}
} // namespace

TEST("parallel - a failing check names what ran beside it", no_scheduler)
{
    CHECK(failure_names_its_partner());
}

TEST("parallel - a thread's running-test slot is freed when the thread ends", no_scheduler)
{
    // A thread claims a slot the first time it runs a test, and each run below runs one on a thread that then ends.
    // Together they are more threads than the table has slots, so a slot claimed for good would leave none free.
    auto reg = nx::test_registry();
    reg.add_declaration("trivial", {}, [] { CHECK(true); });
    auto const schedule = nx::test_schedule::create({}, reg);
    for (auto i = 0; i < 100; ++i)
    {
        std::thread t([&] { (void)nx::execute_tests(schedule, with_jobs(1)); });
        t.join();
    }

    CHECK(failure_names_its_partner());
}

TEST("parallel - tests under -jN really do overlap", no_scheduler)
{
#ifdef __EMSCRIPTEN__
    // A Web Worker starts only once the thread that asked for it returns to the browser's event loop, and the bodies
    // below spin on that thread — so the second worker cannot come up while the first test is waiting for it.
    // The overlap this asserts is real on every other platform, and unreachable in a page by construction.
    SKIP("a wasm worker cannot start while the main thread spins");
#else
    // Every test waits for a second one to join it, with a deadline so a scheduler that genuinely serializes fails the
    // CHECK instead of hanging.
    //
    // **The wait yields rather than spins, and that is the difference between overlapping and not.**
    // `cc::spin_pause` keeps the thread runnable — spin.hh says so, and says it is never a substitute for blocking —
    // so on a machine with fewer free cores than jobs the first body holds the core the second one needs, and the
    // overlap this asserts cannot happen however long the spin runs.
    // It failed exactly that way on a loaded CI runner, as `1 >= 2`.
    cc::atomic<int> live = {0};
    cc::atomic<int> peak = {0};

    // One absolute deadline for the whole run rather than one per body, so a scheduler that never overlaps costs this
    // once instead of once per test.
    auto const deadline = cc::current_time_steady_secs() + 10.0;

    nx::test_registry reg;
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(
            cc::format("t{}", i), {},
            [&live, &peak, deadline]
            {
                auto const now = live.fetch_add(1, cc::memory_order_acq_rel) + 1;
                for (auto observed = peak.load(cc::memory_order_acquire); observed < now;)
                    if (peak.compare_exchange_weak(observed, now, cc::memory_order_acq_rel))
                        break;

                // Waits on `peak` rather than on `live`: the property holds once ANY two bodies have overlapped.
                // So every later body proceeds at once, the last one included — and it is the one with no partner
                // left to wait for.
                // Only a run that never overlaps at all pays the deadline.
                while (peak.load(cc::memory_order_acquire) < 2 && cc::current_time_steady_secs() < deadline)
                    cc::this_thread_yield();

                live.fetch_sub(1, cc::memory_order_acq_rel);
                CHECK(true);
            });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(peak.load(cc::memory_order_acquire) >= 2);
#endif
}
#endif

// ---- async tests under every scheduling ask ----
// Each body is a coroutine taking a pointer by value, so what it records lives in the frame rather than in a capture.

namespace
{
struct segment_record
{
    cc::atomic<u64> first = {0};  // thread of the first segment
    cc::atomic<u64> after = {0};  // thread after an await completed off this thread
    cc::atomic<u64> hopped = {0}; // thread after hopping to compute
    cc::atomic<cc::async_scheduler*> first_scheduler = {nullptr};
    cc::atomic<cc::async_scheduler*> after_scheduler = {nullptr};
};

u64 thread_now()
{
    return u64(cc::current_thread_id());
}

cc::shared_async<cc::unit> record_main_segments(segment_record* r)
{
    r->first.store(thread_now());
    auto const elsewhere = cc::make_async_scheduled_on(cc::compute_scheduler(), [] { return 1; });
    CHECK(co_await elsewhere == 1);
    r->after.store(thread_now());
    co_await cc::async_resume_on_compute();
    r->hopped.store(thread_now());
}

cc::shared_async<cc::unit> hop_to_main_without_the_flag(segment_record* r)
{
    co_await cc::async_resume_on_main();
    r->first.store(thread_now());
    CHECK(true);
}

cc::shared_async<cc::unit> record_schedulers(segment_record* r)
{
    r->first_scheduler.store(cc::async_scheduler::current_or_null());
    r->first.store(thread_now());
    auto const dep = cc::make_async_lazy([] { return 2; });
    CHECK(co_await dep == 2);
    r->after_scheduler.store(cc::async_scheduler::current_or_null());
    r->after.store(thread_now());
}

nx::test_schedule_execution run_one_async(nx::config::cfg cfg,
                                          segment_record& r,
                                          cc::shared_async<cc::unit> (*body)(segment_record*),
                                          int jobs)
{
    nx::test_registry reg;
    reg.add_declaration("busy", {}, [] { CHECK(true); });
    reg.add_async_declaration(
        "subject", cfg, [&r, body](nx::impl::async_test_sink& sink) { nx::impl::submit_test_async(sink, body(&r)); });
    auto const schedule = nx::test_schedule::create({}, reg);
    return nx::execute_tests(schedule, with_jobs(jobs));
}
} // namespace

TEST("parallel - a main_thread async test runs every segment on main until it hops away", no_scheduler)
{
    REQUIRE(cc::current_thread_id() == cc::thread_id::main);
    for (auto const jobs : {1, 4})
    {
        segment_record r;
        auto const exec = run_one_async(nx::impl::merge_config(nx::config::main_thread), r, &record_main_segments, jobs);
        CHECK(exec.count_failed_tests() == 0);
        CHECK(r.first.load() == u64(cc::thread_id::main));
        CHECK(r.after.load() == u64(cc::thread_id::main)); // resumed at home, whichever thread finished the dependency
        CHECK(r.hopped.load() != 0);
    }
}

TEST("parallel - an async test that hops to main completes under -j1", no_scheduler)
{
    REQUIRE(cc::current_thread_id() == cc::thread_id::main);
    segment_record r;
    auto const exec = run_one_async({}, r, &hop_to_main_without_the_flag, 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(r.first.load() == u64(cc::thread_id::main));
}

TEST("parallel - a singlethreaded async test runs inline on the run thread", no_scheduler)
{
    for (auto const jobs : {1, 4})
    {
        segment_record r;
        auto const exec = run_one_async(nx::impl::merge_config(nx::config::singlethreaded), r, &record_schedulers, jobs);
        CHECK(exec.count_failed_tests() == 0);
        CHECK(r.first.load() == thread_now());
        CHECK(r.after.load() == thread_now()); // every segment, and the dependency it awaited, on this one thread
        CHECK(r.first_scheduler.load() != nullptr);
        CHECK(r.first_scheduler.load() == r.after_scheduler.load());
    }
}

TEST("parallel - an own_pool async test keeps every segment on its private pool", no_scheduler)
{
    for (auto const jobs : {1, 4})
    {
        segment_record r;
        auto const exec = run_one_async(nx::impl::merge_config(nx::config::own_pool(2)), r, &record_schedulers, jobs);
        CHECK(exec.count_failed_tests() == 0);
        CHECK(r.first_scheduler.load() != nullptr);
        CHECK(r.first_scheduler.load() == r.after_scheduler.load());
    }
}
