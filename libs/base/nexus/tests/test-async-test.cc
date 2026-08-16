#include <clean-core/common/utility.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// ASYNC_TEST: a body that hands its graph back instead of running to completion.
//
// The contract worth pinning is attribution across the suspension.
// The wrapper node installs the test's ambient link and schedules the returned root under it, so a check reported deep
// inside the graph — on any worker, at any depth — is still billed to this test.

ASYNC_TEST("async test - a check inside the returned graph is billed to this test")
{
    return cc::make_async_lazy<cc::unit>(
        [](cc::async_context<cc::unit>& actx) -> cc::async_step_status
        {
            CHECK(1 + 1 == 2);
            return actx.resolve_to_value(cc::unit{});
        });
}

ASYNC_TEST("async test - a check below a parked dependency is billed to this test too")
{
    // Two levels: the outer frame parks on a dependency it builds on the fly, and both report checks.
    auto inner = cc::make_async_lazy<int>(
        [](cc::async_context<int>& actx) -> cc::async_step_status
        {
            CHECK(true); // reported from a node nexus never saw
            return actx.resolve_to_value(7);
        });

    return cc::make_async_lazy<cc::unit>(
        [inner, waited = false](cc::async_context<cc::unit>& actx) mutable -> cc::async_step_status
        {
            if (!waited)
            {
                waited = true;
                if (!actx.require(inner))
                    return actx.wait_for_dependencies();
            }
            CHECK(*inner->value_ptr() == 7);
            return actx.resolve_to_value(cc::unit{});
        });
}

namespace
{
// Runs `body` as a nested async test and returns the run's result.
// Every test below that nests a run is no_scheduler, as any nesting test must be.
nx::test_schedule_execution run_async(cc::unique_function<void(nx::impl::async_test_sink&)> body, int jobs = 1)
{
    nx::test_registry reg;
    reg.add_async_declaration("subject", {}, cc::move(body));

    auto const schedule = nx::test_schedule::create({}, reg);
    nx::test_schedule_config config;
    config.jobs = jobs;
    return nx::execute_tests(schedule, config);
}
} // namespace

TEST("async test - a graph that resolves to an error fails the test, without propagating", no_scheduler)
{
    auto const exec = run_async(
        [](nx::impl::async_test_sink& sink)
        {
            CHECK(true); // so the test is not failed merely for having no checks
            nx::impl::submit_test_async(
                sink, cc::make_async_lazy<cc::unit>([](cc::async_context<cc::unit>& actx) -> cc::async_step_status
                                                    { return actx.error(cc::any_error("deliberate")); }));
        });

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].is_considered_failing());
    CHECK(exec.orphan_checks == 0);

    // The wrapper resolves to a VALUE whatever the graph did, so the failure stays this test's.
    // ../docs/parallel-execution.md has why that is load-bearing rather than tidiness.
    auto const& errors = exec.executions[0].root.errors;
    REQUIRE(errors.size() >= 1);
    CHECK(errors[0].expanded.contains("deliberate"));
}

TEST("async test - work left running past the graph still fails the test by name", no_scheduler)
{
    auto const exec = run_async(
        [](nx::impl::async_test_sink& sink)
        {
            nx::impl::submit_test_async(sink, cc::make_async_lazy<cc::unit>(
                                                  [](cc::async_context<cc::unit>& actx) -> cc::async_step_status
                                                  {
                                                      // Scheduled under this test's context and then abandoned: never awaited, never cancelled.
                                                      // It is the run's scheduler that now holds it, which is exactly the interference the check exists to find.
                                                      auto abandoned = cc::make_async_lazy<int>(
                                                          [](cc::async_context<int>& inner) -> cc::async_step_status
                                                          { return inner.resolve_to_value(1); });
                                                      abandoned->schedule();

                                                      CHECK(true);
                                                      return actx.resolve_to_value(cc::unit{});
                                                  }));
        });

    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].is_considered_failing());

    auto const& errors = exec.executions[0].root.errors;
    auto found = false;
    for (auto const& e : errors)
        found |= e.expr.contains("left async work running");
    CHECK(found);
}

TEST("async test - checks land on the right test when async and plain tests interleave under -jN", no_scheduler)
{
    nx::test_registry reg;
    for (auto i = 1; i <= 6; ++i)
    {
        reg.add_declaration(cc::format("plain{}", i), {},
                            [i]
                            {
                                for (auto k = 0; k < i; ++k)
                                    CHECK(true);
                            });
        reg.add_async_declaration(cc::format("async{}", i), {},
                                  [i](nx::impl::async_test_sink& sink)
                                  {
                                      nx::impl::submit_test_async(
                                          sink, cc::make_async_lazy<cc::unit>(
                                                    [i](cc::async_context<cc::unit>& actx) -> cc::async_step_status
                                                    {
                                                        for (auto k = 0; k < i; ++k)
                                                            CHECK(true);
                                                        return actx.resolve_to_value(cc::unit{});
                                                    }));
                                  });
    }

    auto const schedule = nx::test_schedule::create({}, reg);
    nx::test_schedule_config config;
    config.jobs = 4;
    auto const exec = nx::execute_tests(schedule, config);

    REQUIRE(exec.executions.size() == 12);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.orphan_checks == 0);

    // A misattribution shows up as a wrong per-test count, which a correct total would hide.
    for (auto i = 1; i <= 6; ++i)
    {
        CHECK(exec.executions[(i - 1) * 2].root.executed_checks == i);
        CHECK(exec.executions[(i - 1) * 2 + 1].root.executed_checks == i);
    }
}
