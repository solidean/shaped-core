#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// nx::impl::test_run: a run driven in steps, for a host whose results arrive only after the current call returns.
//
// The contract worth pinning is the stall: a step that finds nothing it can progress returns instead of parking, and the
// run picks up where it left off once something outside it delivered.

namespace
{
cc::shared_async<cc::unit> check_n_times(int n)
{
    for (auto k = 0; k < n; ++k)
        CHECK(true);
    co_return;
}

// Steps `run` until it finishes, failing loudly rather than spinning if it never does.
bool step_to_end(nx::impl::test_run& run)
{
    for (auto i = 0; i < 10'000; ++i)
        if (run.step())
            return true;
    return false;
}
} // namespace

TEST("test_run - stepping a schedule reaches the result execute_tests does", no_scheduler)
{
    nx::test_registry reg;
    for (auto i = 1; i <= 4; ++i)
    {
        reg.add_declaration(cc::format("plain{}", i), {},
                            [i]
                            {
                                for (auto k = 0; k < i; ++k)
                                    CHECK(true);
                            });
        reg.add_async_declaration(cc::format("async{}", i), {}, [i](nx::impl::async_test_sink& sink)
                                  { nx::impl::submit_test_async(sink, check_n_times(i)); });
    }

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const config = nx::test_schedule_config{};
    auto run = nx::impl::test_run(schedule, config);
    REQUIRE(step_to_end(run));

    auto const exec = run.take_result();
    REQUIRE(exec.executions.size() == 8);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.orphan_checks == 0);
    for (auto i = 1; i <= 4; ++i)
    {
        CHECK(exec.executions[(i - 1) * 2].root.executed_checks == i);
        CHECK(exec.executions[(i - 1) * 2 + 1].root.executed_checks == i);
    }
}

TEST("test_run - a step stalls on what only the host delivers, and the run finishes once it arrives", no_scheduler)
{
    // The node stands in for a WebGPU readback: nothing a step can drive settles it.
    auto const delivered = cc::make_async_manual<int>();

    nx::test_registry reg;
    reg.add_async_declaration("waits on the host", {},
                              [delivered](nx::impl::async_test_sink& sink)
                              {
                                  nx::impl::submit_test_async(
                                      sink, [](cc::shared_async<int> value) -> cc::shared_async<cc::unit>
                                      { CHECK(co_await value == 42); }(delivered));
                              });
    reg.add_declaration("runs after it", {}, [] { CHECK(true); });

    auto const schedule = nx::test_schedule::create({.shuffle = false}, reg);
    auto const config = nx::test_schedule_config{.shuffle = false};
    auto run = nx::impl::test_run(schedule, config);

    CHECK(!run.step());
    CHECK(!run.step()); // still nothing to do, and still no park

    delivered->push_value(42);
    REQUIRE(step_to_end(run));

    auto const exec = run.take_result();
    REQUIRE(exec.executions.size() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_total_checks() == 2);
}
