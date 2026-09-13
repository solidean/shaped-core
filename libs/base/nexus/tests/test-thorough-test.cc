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

TEST("thorough - the flag parses from the command line, and is off without it")
{
    char const* const plain[] = {"prog"};
    char const* const thorough[] = {"prog", "--thorough"};

    CHECK(!nx::test_schedule_config::create_from_args(1, const_cast<char**>(plain)).thorough);
    CHECK(nx::test_schedule_config::create_from_args(2, const_cast<char**>(thorough)).thorough);
}
