#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

TEST("thorough - a body reads the flag of the run it is part of", no_scheduler)
{
    auto seen = cc::vector<bool>();

    nx::test_registry reg;
    reg.add_declaration("probe", {},
                        [&]
                        {
                            seen.push_back(nx::is_thorough());
                            SUCCEED();
                        });

    auto config = nx::test_schedule_config{};
    auto schedule = nx::test_schedule::create(config, reg);
    [[maybe_unused]] auto const narrowed = nx::execute_tests(schedule, config);

    config.thorough = true;
    [[maybe_unused]] auto const full = nx::execute_tests(schedule, config);

    REQUIRE(seen.size() == 2);
    CHECK(!seen[0]);
    CHECK(seen[1]);
}

TEST("thorough - a thorough_only test is skipped by default and runs under --thorough", no_scheduler)
{
    auto runs = 0;

    nx::test_registry reg;
    reg.add_declaration("full strength", nx::impl::merge_config(nx::config::thorough_only),
                        [&]
                        {
                            ++runs;
                            CHECK(nx::is_thorough());
                        });

    auto config = nx::test_schedule_config{};
    auto schedule = nx::test_schedule::create(config, reg);

    // Skipped rather than left out: the test is still part of the run, and its skip is not a failure.
    auto const narrowed = nx::execute_tests(schedule, config);
    CHECK(runs == 0);
    CHECK(narrowed.count_total_tests() == 1);
    CHECK(narrowed.count_failed_tests() == 0);

    config.thorough = true;
    auto const full = nx::execute_tests(schedule, config);
    CHECK(runs == 1);
    CHECK(full.count_failed_tests() == 0);
}

TEST("thorough - a thorough_only invocable is skipped at dispatch, whatever its driver carries", no_scheduler)
{
    struct probe_key
    {
    };
    auto runs = 0;

    nx::test_registry reg;
    reg.add_invocable_declaration("full strength child", nx::impl::merge_config(nx::config::thorough_only),
                                  cc::arg_types_of(cc::signature<void(probe_key)>{}),
                                  [&](cc::span<nx::typed_value*>)
                                  {
                                      ++runs;
                                      CHECK(true);
                                  });
    reg.add_declaration("driver", {}, [] { nx::invoke_tests("run", probe_key{}); });

    auto config = nx::test_schedule_config{};
    auto schedule = nx::test_schedule::create(config, reg);

    auto const narrowed = nx::execute_tests(schedule, config);
    CHECK(runs == 0);
    CHECK(narrowed.count_failed_tests() == 0);

    config.thorough = true;
    [[maybe_unused]] auto const full = nx::execute_tests(schedule, config);
    CHECK(runs == 1);
}

TEST("thorough - the flag parses from the command line, and is off without it")
{
    char const* const plain[] = {"prog"};
    char const* const thorough[] = {"prog", "--thorough"};

    CHECK(!nx::test_schedule_config::create_from_args(1, const_cast<char**>(plain)).thorough);
    CHECK(nx::test_schedule_config::create_from_args(2, const_cast<char**>(thorough)).thorough);
}
