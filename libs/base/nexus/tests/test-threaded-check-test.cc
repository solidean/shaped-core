#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>
#include <nexus/tests/thread_scope.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

using cc::async_context;

// Checks reported from somewhere other than the test's own thread.
//
// Every test here runs an INNER registry through nx::execute_tests and asserts on the result, so the outer test stays green while the inner one fails on purpose.
// What is being pinned throughout is attribution: which test a check is counted against, and what happens when the answer is "none".
//
// Not gated on CC_HAS_THREADS as a whole: without threads the pool drives graphs inline on the caller, and the answers must be identical either way.
// Only what genuinely needs a second thread is gated.

TEST("threaded check - a check inside a pool-driven node counts towards its test", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("worker_check", {},
                        []
                        {
                            cc::async_thread_pool pool(4);
                            auto n = cc::make_async_lazy<int>(
                                [](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    CHECK(1 + 1 == 2); // on a worker: no test context on that thread, only the node's
                                    return ctx.success(7);
                                });
                            CHECK(pool.blocking_get(n) == 7);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_checks() == 2); // the worker's check was counted, not dropped
    CHECK(exec.count_failed_checks() == 0);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.orphan_checks == 0);
}

TEST("threaded check - a failing check inside a pool-driven node fails its test", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("worker_check_fails", {},
                        []
                        {
                            cc::async_thread_pool pool(4);
                            auto n = cc::make_async_lazy<int>(
                                [](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    CHECK(1 == 2);
                                    return ctx.success(0);
                                });
                            (void)pool.blocking_get(n);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_failed_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("threaded check - a REQUIRE inside a node fails the test and resolves the node on the error channel", no_scheduler)
{
    nx::test_registry reg;
    bool node_errored = false;
    bool past_require = false;
    reg.add_declaration("worker_require", {},
                        [&]
                        {
                            cc::async_thread_pool pool(4);
                            auto n = cc::make_async_lazy<int>(
                                [&](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    REQUIRE(1 == 2);
                                    past_require = true;
                                    return ctx.success(0);
                                });
                            node_errored = pool.try_blocking_get(n).has_error();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(!past_require); // REQUIRE still aborts, by throwing
    CHECK(node_errored);  // and cc::async contains that throw as the node's failure rather than terminating
    CHECK(exec.count_failed_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("threaded check - a section cannot be opened off the test's own thread", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("worker_section", {},
                        []
                        {
                            cc::async_thread_pool pool(4);
                            auto n = cc::make_async_lazy<int>(
                                [](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    SECTION("nope")
                                    {
                                        CHECK(false);
                                    }
                                    return ctx.success(1);
                                });
                            CHECK(pool.blocking_get(n) == 1);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // The section is refused rather than entered, so the body inside it never runs and the misuse is what fails the test.
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("threaded check - a guide metric from a node lands on its test", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("worker_metric", {},
                        []
                        {
                            cc::async_thread_pool pool(4);
                            auto n = cc::make_async_lazy<int>(
                                [](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    nx::guide::report_raw("from-a-worker", 1.0, "1");
                                    return ctx.success(1);
                                });
                            CHECK(pool.blocking_get(n) == 1);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].metrics.size() == 1);
    CHECK(exec.executions[0].metrics[0].name == "from-a-worker");
}

TEST("threaded check - a test that leaks async work fails, naming itself", no_scheduler)
{
    nx::test_registry reg;
    cc::singlethreaded_scheduler sched; // queued work stays queued until someone drains it
    reg.add_declaration("leaks_async_work", {},
                        [&]
                        {
                            CHECK(true);
                            cc::async_worker_scope const ws(sched);
                            auto n = cc::make_async_lazy<int>([](async_context<int>& ctx) -> cc::async_step_status
                                                              { return ctx.success(1); });
                            n->schedule(); // queued under this test's context, never awaited
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_failed_tests() == 1); // leaked work would report into whatever runs next — that is interference
}

TEST("threaded check - a node of one test driven inside another is never billed to the wrong one", no_scheduler)
{
    // The case a thread-local stack cannot get right: "consumer" drives a node belonging to "producer", on its own thread, with its own test on its own stack.
    // A stack read would bill it to consumer.
    cc::singlethreaded_scheduler sched;

    nx::test_registry reg;
    reg.add_declaration("producer", {},
                        [&]
                        {
                            CHECK(true);
                            cc::async_worker_scope const ws(sched);
                            auto n = cc::make_async_lazy<int>(
                                [](async_context<int>& ctx) -> cc::async_step_status
                                {
                                    CHECK(1 == 2);
                                    return ctx.success(1);
                                });
                            n->schedule(); // queued under producer's context, never awaited
                        });
    reg.add_declaration("consumer", {},
                        [&]
                        {
                            CHECK(true);
                            cc::async_worker_scope const ws(sched);
                            sched.drain(); // runs producer's node here, on consumer's thread
                        });

    // Serialized on purpose: the point is that consumer drives the node AFTER producer has finished.
    // Run in parallel the two overlap, the node is billed to a still-live producer, and there is no orphan to observe.
    nx::test_schedule_config config;
    config.jobs = 1;

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    REQUIRE(exec.executions.size() == 2);
    CHECK(exec.executions[0].is_considered_failing());  // producer, for leaving the work behind
    CHECK(!exec.executions[1].is_considered_failing()); // consumer, which merely ran it

    // The node's check ran after producer had finished, so it belongs to no live test and is an orphan — never consumer's.
    CHECK(exec.orphan_checks == 1);
}

#if CC_HAS_THREADS
TEST("threaded check - a check on a bare thread is an orphan, and fails the run", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("bare_thread", {},
                        []
                        {
                            CHECK(true);
                            std::thread t([] { CHECK(true); }); // true, and still an error: nothing can attribute it
                            t.join();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.orphan_checks == 1); // a passing check that proved nothing is exactly what must not pass quietly
    CHECK(exec.orphan_errors.size() == 1);
    CHECK(exec.count_total_checks() == 1); // and it was counted towards no test
    CHECK(exec.count_failed_tests() == 0);
}

TEST("threaded check - attributed_to_current_test rescues a bare thread", no_scheduler)
{
    nx::test_registry reg;
    reg.add_declaration("attributed_thread", {},
                        []
                        {
                            std::thread t(nx::attributed_to_current_test([] { CHECK(1 == 2); }));
                            t.join();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.orphan_checks == 0);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_checks() == 1); // attributed, so its failure reaches the right test
    CHECK(exec.count_failed_tests() == 1);
}

TEST("threaded check - a REQUIRE on an attributed bare thread records instead of throwing", no_scheduler)
{
    nx::test_registry reg;
    bool past_require = false;
    reg.add_declaration("attributed_require", {},
                        [&]
                        {
                            std::thread t(nx::attributed_to_current_test(
                                [&]
                                {
                                    REQUIRE(1 == 2);
                                    past_require = true;
                                }));
                            t.join();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Nothing stands between here and the thread function, so throwing would terminate the process rather than abort a test.
    CHECK(past_require);
    CHECK(exec.count_failed_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

#endif
