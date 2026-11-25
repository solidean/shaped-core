#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

TEST("known issues - a skip by one stops the test, and --known-issues runs it on", no_scheduler)
{
    auto reached = 0;

    nx::test_registry reg;
    reg.add_declaration("probe", {},
                        [&]
                        {
                            nx::skip_known_issue("some-driver-bug", "draws nothing");
                            ++reached;
                            CHECK(nx::runs_known_issues());
                        });

    auto config = nx::test_schedule_config{};
    auto schedule = nx::test_schedule::create(config, reg);

    // Skipped rather than failed: the default run keeps a known bug from turning the run red.
    auto const skipped = nx::execute_tests(schedule, config);
    CHECK(reached == 0);
    CHECK(skipped.count_total_tests() == 1);
    CHECK(skipped.count_failed_tests() == 0);

    // Tried: the body runs past the skip, so whether the bug still reproduces is the test's own verdict.
    config.run_known_issues = true;
    auto const tried = nx::execute_tests(schedule, config);
    CHECK(reached == 1);
    CHECK(tried.count_failed_tests() == 0);
}
