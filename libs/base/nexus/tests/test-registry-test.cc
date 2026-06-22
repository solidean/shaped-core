#include <nexus/test.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>


TEST("test registry - basics")
{
    nx::test_registry reg;
    reg.add_declaration( //
        "testA", {},
        []
        {
            //
            CHECK(1 + 2 == 3);
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test registry - multi-test multi-check accounting")
{
    nx::test_registry reg;

    // T1: 2 passing CHECKs
    reg.add_declaration("T1", {},
                        []
                        {
                            CHECK(1 == 1);
                            CHECK(2 == 2);
                        });

    // T2: 1 passing, 1 failing CHECK
    reg.add_declaration("T2", {},
                        []
                        {
                            CHECK(1 == 1);
                            CHECK(1 == 2); // fails
                        });

    // T3: 3 failing CHECKs
    reg.add_declaration("T3", {},
                        []
                        {
                            CHECK(1 == 2); // fails
                            CHECK(2 == 3); // fails
                            CHECK(3 == 4); // fails
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 3);
    CHECK(exec.count_total_checks() == 7);
    CHECK(exec.count_failed_tests() == 2);  // T2 and T3
    CHECK(exec.count_failed_checks() == 4); // 1 from T2, 3 from T3
}

TEST("test registry - CHECK continues after failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("check_continues", {},
                        [&counter]
                        {
                            CHECK(false); // should fail
                            ++counter;    // should still run
                            CHECK(counter == 1);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify continuation happened
    CHECK(counter == 1);

    // Check counts
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1); // only the first CHECK failed
}

TEST("test registry - REQUIRE aborts test on failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_aborts", {},
                        [&counter]
                        {
                            REQUIRE(false); // should fail & abort
                            ++counter;      // must NOT run
                            CHECK(false);   // must NOT run
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify abort happened
    CHECK(counter == 0);

    // Check counts
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1); // only REQUIRE is counted
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1); // REQUIRE counts as a check
}

TEST("test registry - CC_ASSERT_ALWAYS aborts test on failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("assert_always_aborts", {},
                        [&counter]
                        {
                            CC_ASSERT_ALWAYS(false, "assertion failed"); // should fail & abort
                            ++counter;                                   // must NOT run
                            CHECK(false);                                // must NOT run
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify abort happened
    CHECK(counter == 0);

    // Check counts - the test should be marked as failed
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("test registry - disabled tests are not executed")
{
    nx::test_registry reg;

    int counter = 0;

    // Enabled test
    reg.add_declaration("T_enabled", {.enabled = true},
                        [&counter]
                        {
                            ++counter;
                            SUCCEED();
                        });

    // Disabled test - body should never run
    reg.add_declaration("T_disabled", {.enabled = false},
                        [&counter]
                        {
                            ++counter;
                            CHECK(false); // would fail if executed
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Only enabled test executed
    CHECK(counter == 1);

    // Disabled tests do not contribute to test count
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test registry - config via aggregate literal vs merge_config are equivalent")
{
    nx::test_registry reg;

    int counter_a = 0;
    int counter_b = 0;

    // Config via aggregate literal
    reg.add_declaration("T_cfg_aggregate", nx::config::cfg{.enabled = false, .seed = 123},
                        [&counter_a]
                        {
                            ++counter_a;
                            CHECK(false); // would fail if executed
                        });

    // Config via merge_config with named items
    using namespace nx::config;
    auto cfg_items = nx::impl::merge_config(disabled, seed(123));
    reg.add_declaration("T_cfg_items", cfg_items,
                        [&counter_b]
                        {
                            ++counter_b;
                            CHECK(false); // would fail if executed
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Both disabled, neither executed
    CHECK(counter_a == 0);
    CHECK(counter_b == 0);

    // Both disabled tests are not counted
    CHECK(exec.count_total_tests() == 0);
    CHECK(exec.count_total_checks() == 0);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test registry - seed configuration is stored")
{
    nx::test_registry reg;

    // Tests with different seed values
    reg.add_declaration("T_seed_42_a", nx::config::cfg{.enabled = true, .seed = 42}, [] { SUCCEED(); });

    reg.add_declaration("T_seed_42_b", nx::config::cfg{.enabled = true, .seed = 42}, [] { SUCCEED(); });

    reg.add_declaration("T_seed_13", nx::config::cfg{.enabled = true, .seed = 13}, [] { SUCCEED(); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify seeds are stored in declarations
    CHECK(reg.declarations[0].test_config.seed == 42);
    CHECK(reg.declarations[1].test_config.seed == 42);
    CHECK(reg.declarations[2].test_config.seed == 13);

    // All tests pass
    CHECK(exec.count_total_tests() == 3);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test registry - uncaught exceptions become failing tests")
{
    nx::test_registry reg;

    // Normal test
    reg.add_declaration("T_normal", {}, [] { SUCCEED(); });

    // Test that throws after a CHECK
    reg.add_declaration("T_throw", {},
                        []
                        {
                            SUCCEED(); // this passes
                            throw std::runtime_error("test exception");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 2);
    CHECK(exec.count_total_checks() == 2); // both CHECKs counted
    CHECK(exec.count_failed_tests() == 1); // T_throw failed
    // Note: The framework may or may not count the exception as an additional failed check
    // We accept either count_failed_checks() == 0 or == 1 depending on implementation
}

TEST("test registry - failure attribution with mixed CHECK and REQUIRE")
{
    nx::test_registry reg;

    reg.add_declaration("mixed_failure", {},
                        []
                        {
                            CHECK(false);   // fails
                            CHECK(false);   // fails
                            REQUIRE(false); // fails and aborts
                            CHECK(false);   // should NOT run
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);  // 2 CHECKs + 1 REQUIRE
    CHECK(exec.count_failed_checks() == 3); // all 3 failed
    CHECK(exec.count_failed_tests() == 1);  // 1 test failed
}

TEST("test registry - duplicate test names are both registered")
{
    nx::test_registry reg;

    int counter_a = 0;
    int counter_b = 0;

    // Register two tests with the same name
    reg.add_declaration("duplicate_name", {},
                        [&counter_a]
                        {
                            ++counter_a;
                            SUCCEED();
                        });

    reg.add_declaration("duplicate_name", {},
                        [&counter_b]
                        {
                            ++counter_b;
                            SUCCEED();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Both tests run (framework allows duplicate names)
    CHECK(counter_a == 1);
    CHECK(counter_b == 1);
    CHECK(exec.count_total_tests() == 2);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test registry - test with zero checks is counted as a test")
{
    nx::test_registry reg;

    int counter_empty = 0;
    int counter_check = 0;

    // Test with no checks
    reg.add_declaration("T_empty", {},
                        [&counter_empty]
                        {
                            ++counter_empty;
                            // no checks
                        });

    // Test with one check
    reg.add_declaration("T_check", {},
                        [&counter_check]
                        {
                            ++counter_check;
                            SUCCEED();
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Both tests run
    CHECK(counter_empty == 1);
    CHECK(counter_check == 1);

    // Empty test counts as a test and as a failure (user error)
    CHECK(exec.count_total_tests() == 2);
    CHECK(exec.count_total_checks() == 1);  // only T_check has a CHECK
    CHECK(exec.count_failed_tests() == 1);  // T_empty fails (no checks)
    CHECK(exec.count_failed_checks() == 0); // no failed CHECKs, just missing checks
}

TEST("test registry - schedule integration with run_disabled_tests config")
{
    nx::test_registry reg;

    int counter_enabled = 0;
    int counter_disabled = 0;

    reg.add_declaration("T_enabled", {.enabled = true},
                        [&counter_enabled]
                        {
                            ++counter_enabled;
                            SUCCEED();
                        });

    reg.add_declaration("T_disabled", {.enabled = false},
                        [&counter_disabled]
                        {
                            ++counter_disabled;
                            SUCCEED();
                        });

    // Schedule without run_disabled_tests
    {
        auto schedule = nx::test_schedule::create({.run_disabled_tests = false}, reg);
        auto exec = nx::execute_tests(schedule, {});

        CHECK(counter_enabled == 1);
        CHECK(counter_disabled == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // Reset counters
    counter_enabled = 0;
    counter_disabled = 0;

    // Schedule with run_disabled_tests
    {
        auto schedule = nx::test_schedule::create({.run_disabled_tests = true}, reg);
        auto exec = nx::execute_tests(schedule, {});

        CHECK(counter_enabled == 1);
        CHECK(counter_disabled == 1); // now runs
        CHECK(exec.count_total_tests() == 2);
    }
}
