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
    // On the error channel it would propagate into every node ordered behind it.
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

TEST("parallel - exclusive tag holders never overlap, and run in schedule order", no_scheduler)
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

    // An edge fixes the order, not merely the exclusion — so the holders run in the order the schedule listed them.
    REQUIRE(gpu_order.size() == 6);
    for (auto i = 0; i < 6; ++i)
        CHECK(gpu_order[i] == cc::format("g{}", i));
}

TEST("parallel - a no-arg exclusive test runs alone", no_scheduler)
{
    cc::atomic<int> live = {0};
    cc::atomic<int> seen_beside_the_barrier = {0};
    cc::atomic<bool> barrier_running = {false};

    nx::test_registry reg;
    auto const busy = [&]
    {
        live.fetch_add(1, cc::memory_order_acq_rel);
        if (barrier_running.load(cc::memory_order_acquire))
            seen_beside_the_barrier.fetch_add(1, cc::memory_order_relaxed);
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
                            barrier_running.store(true, cc::memory_order_release);
                            CHECK(live.load(cc::memory_order_acquire) == 0); // nothing before it may still be running
                            for (auto spin = 0; spin < 20000; ++spin)
                                cc::spin_pause();
                            barrier_running.store(false, cc::memory_order_release);
                        });
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("after{}", i), {}, busy);

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(seen_beside_the_barrier.load(cc::memory_order_acquire) == 0);
}

TEST("parallel - a no-arg exclusive test is routed off the scheduler entirely", no_scheduler)
{
    // Running beside nothing is exactly what the no-scheduler group already delivers, so a barrier is scheduled there instead of as a node with an edge to every test before it.
    // Observable as the body landing on the run's OWN calling thread.
    // A plain test may land there too — the caller participates as a worker — so only the barrier's thread is pinned.
    auto const caller = cc::current_thread_id();
    auto barrier_thread = cc::thread_id::invalid;

    nx::test_registry reg;
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("busy{}", i), {}, [] { CHECK(true); });
    reg.add_declaration("alone", nx::impl::merge_config(nx::config::exclusive()),
                        [&]
                        {
                            barrier_thread = cc::current_thread_id();
                            CHECK(true);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(barrier_thread == caller);
}

// The routing above must not reach an async body: with no scheduler bound, nothing would drive the root it hands back.
// This test is the canary — it only passes if an exclusive ASYNC_TEST kept its scheduler.
ASYNC_TEST("parallel - an exclusive ASYNC_TEST still gets a scheduler", exclusive())
{
    return cc::make_async_lazy<cc::unit>(
        [](cc::async_context<cc::unit>& actx) -> cc::async_step_status
        {
            CHECK(true);
            return actx.resolve_to_value(cc::unit{});
        });
}

// nx::main_thread — a flag rather than a fourth scheduler mode, so it composes with the modes instead of excluding them.
// The outer body here runs on the run's own calling thread, which nx::run claimed as main, so a nested run can honour the flag.

TEST("parallel - a main_thread test runs on the process main thread", no_scheduler)
{
    // Stronger than the barrier test above, which only pins the body to "whoever called": this compares against cc::thread_id::main.
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

TEST("parallel - main_thread and no_scheduler share one phase, in schedule order", no_scheduler)
{
    // Two separate promises — a thread, and an absent scheduler — that today land in the same group.
    // Pinning the order is what makes replacing that implementation a visible change rather than a silent one.
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
        CHECK(order[i * 2] == cc::format("m{}", i));
        CHECK(order[i * 2 + 1] == cc::format("n{}", i));
    }
}

TEST("parallel - main_thread composes with exclusive()", no_scheduler)
{
    // Both route into the same group; the guard is that asking for both still runs the body once, on main, beside nothing.
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
TEST("parallel - a failing check names what ran beside it", no_scheduler)
{
    // A -jN failure is usually about what it ran BESIDE, and the report otherwise names only the test that failed.
    // The overlap is forced rather than hoped for: the failing test waits for a partner to be live, and the partner stays live until it has failed.
    // Both waits are bounded, so a machine that refuses to overlap fails the CHECK below instead of hanging.
    cc::atomic<int> live = {0};
    cc::atomic<bool> has_failed = {false};

    nx::test_registry reg;
    reg.add_declaration("partner", {},
                        [&]
                        {
                            live.fetch_add(1, cc::memory_order_acq_rel);
                            for (auto spin = 0; spin < 2000000 && !has_failed.load(cc::memory_order_acquire); ++spin)
                                cc::spin_pause();
                            CHECK(true);
                        });
    reg.add_declaration("failing", {},
                        [&]
                        {
                            live.fetch_add(1, cc::memory_order_acq_rel);
                            for (auto spin = 0; spin < 2000000 && live.load(cc::memory_order_acquire) < 2; ++spin)
                                cc::spin_pause();
                            CHECK(false); // the failure under test
                            has_failed.store(true, cc::memory_order_release);
                        });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    REQUIRE(exec.executions.size() == 2);
    auto const& failing = exec.executions[1];
    REQUIRE(failing.root.errors.size() == 1);

    auto beside = false;
    for (auto const& line : failing.root.errors[0].extra_lines)
        if (line.contains("partner"))
            beside = true;
    CHECK(beside);
}

TEST("parallel - tests under -jN really do overlap", no_scheduler)
{
    // Every test waits for a second one to join it, with a bounded fallback so a machine that refuses to overlap fails the CHECK instead of hanging.
    cc::atomic<int> live = {0};
    cc::atomic<int> peak = {0};

    nx::test_registry reg;
    for (auto i = 0; i < 4; ++i)
        reg.add_declaration(cc::format("t{}", i), {},
                            [&live, &peak]
                            {
                                auto const now = live.fetch_add(1, cc::memory_order_acq_rel) + 1;
                                for (auto observed = peak.load(cc::memory_order_acquire); observed < now;)
                                    if (peak.compare_exchange_weak(observed, now, cc::memory_order_acq_rel))
                                        break;

                                for (auto spin = 0; spin < 1000000 && live.load(cc::memory_order_acquire) < 2; ++spin)
                                    cc::spin_pause();

                                live.fetch_sub(1, cc::memory_order_acq_rel);
                                CHECK(true);
                            });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, with_jobs(4));

    CHECK(exec.count_failed_tests() == 0);
    CHECK(peak.load(cc::memory_order_acquire) >= 2);
}
#endif
