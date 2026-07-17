#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <stdexcept>


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

TEST("test registry - manual tests are excluded from automatic and run_disabled sweeps")
{
    nx::test_registry reg;

    int counter_normal = 0;
    int counter_manual = 0;

    reg.add_declaration("T_normal", {},
                        [&]
                        {
                            ++counter_normal;
                            SUCCEED();
                        });
    reg.add_declaration("T_manual", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [&]
                        {
                            ++counter_manual;
                            SUCCEED();
                        });

    // Default schedule: only the normal test runs.
    {
        auto exec = nx::execute_tests(nx::test_schedule::create({}, reg), {});
        CHECK(counter_normal == 1);
        CHECK(counter_manual == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // A bulk "run disabled too" request must NOT sweep in manual tests.
    counter_normal = counter_manual = 0;
    {
        auto exec = nx::execute_tests(nx::test_schedule::create({.run_disabled_tests = true}, reg), {});
        CHECK(counter_manual == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // An exact name pulls a manual test in across the bucket (what a bare filter allows).
    counter_normal = counter_manual = 0;
    {
        auto exec = nx::execute_tests(
            nx::test_schedule::create({.filters = {"T_manual"}, .allow_cross_bucket_naming = true}, reg), {});
        CHECK(counter_normal == 0); // "T_manual" is not a substring of "T_normal"
        CHECK(counter_manual == 1);
        CHECK(exec.count_total_tests() == 1);
    }

    // ...but a SUBSTRING filter must not: "T_" matches both names, yet only the normal-bucket test runs.
    counter_normal = counter_manual = 0;
    {
        auto exec = nx::execute_tests(
            nx::test_schedule::create({.filters = {"T_"}, .allow_cross_bucket_naming = true}, reg), {});
        CHECK(counter_normal == 1);
        CHECK(counter_manual == 0);
        CHECK(exec.count_total_tests() == 1);
    }
}

TEST("test registry - selected_bucket restricts the eligible set to that bucket")
{
    nx::test_registry reg;

    int counter_normal = 0;
    int counter_disabled = 0;
    int counter_manual = 0;

    reg.add_declaration("T_normal", {},
                        [&]
                        {
                            ++counter_normal;
                            SUCCEED();
                        });
    reg.add_declaration("T_disabled", nx::config::cfg{.enabled = false},
                        [&]
                        {
                            ++counter_disabled;
                            SUCCEED();
                        });
    reg.add_declaration("T_manual", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [&]
                        {
                            ++counter_manual;
                            SUCCEED();
                        });

    // --manual mode: only the manual bucket, even disabled (normal-bucket) ones stay out.
    auto exec
        = nx::execute_tests(nx::test_schedule::create({.selected_bucket = nx::config::test_bucket::manual}, reg), {});

    CHECK(counter_normal == 0);
    CHECK(counter_disabled == 0);
    CHECK(counter_manual == 1);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test registry - guide-benchmark bucket is swept only when selected")
{
    nx::test_registry reg;

    int counter_normal = 0;
    int counter_manual = 0;
    int counter_guide = 0;

    reg.add_declaration("T_normal", {},
                        [&]
                        {
                            ++counter_normal;
                            SUCCEED();
                        });
    reg.add_declaration("T_manual", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [&]
                        {
                            ++counter_manual;
                            SUCCEED();
                        });
    reg.add_declaration("T_guide", nx::config::cfg{.bucket = nx::config::test_bucket::guide_benchmark},
                        [&]
                        {
                            ++counter_guide; /* no checks: allowed */
                        });

    // Default sweep: neither manual nor guide benchmarks run.
    {
        auto exec = nx::execute_tests(nx::test_schedule::create({}, reg), {});
        CHECK(counter_normal == 1);
        CHECK(counter_manual == 0);
        CHECK(counter_guide == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // --guide-benchmarks mode: only the guide_benchmark bucket, and an empty-CHECK guide benchmark passes.
    counter_normal = counter_manual = counter_guide = 0;
    {
        auto exec = nx::execute_tests(
            nx::test_schedule::create({.selected_bucket = nx::config::test_bucket::guide_benchmark}, reg), {});
        CHECK(counter_normal == 0);
        CHECK(counter_manual == 0);
        CHECK(counter_guide == 1);
        CHECK(exec.count_total_tests() == 1);
        CHECK(exec.count_failed_tests() == 0);
    }
}

TEST("test registry - manual test without any CHECK is not a failure")
{
    nx::test_registry reg;

    // A benchmark-style manual test that only "prints" — no CHECK/REQUIRE at all.
    reg.add_declaration("T_manual_bench", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [] { /* no checks */ });

    auto exec
        = nx::execute_tests(nx::test_schedule::create({.selected_bucket = nx::config::test_bucket::manual}, reg), {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_failed_tests() == 0); // empty manual test is allowed, unlike a normal empty test
}

TEST("test schedule config - a bucket flag selects a bucket and pins the sweep to it")
{
    // --manual selects the manual bucket. An explicit flag pins the sweep: no cross-bucket naming.
    {
        char a0[] = "prog";
        char a1[] = "--manual";
        char* argv[] = {a0, a1};
        auto const cfg = nx::test_schedule_config::create_from_args(2, argv);
        CHECK(cfg.selected_bucket == nx::config::test_bucket::manual);
        CHECK(!cfg.allow_cross_bucket_naming);
        CHECK(!cfg.run_disabled_tests);
        CHECK(cfg.filters.empty());
    }

    // --guide-benchmarks selects the guide_benchmark bucket.
    {
        char a0[] = "prog";
        char a1[] = "--guide-benchmarks";
        char* argv[] = {a0, a1};
        auto const cfg = nx::test_schedule_config::create_from_args(2, argv);
        CHECK(cfg.selected_bucket == nx::config::test_bucket::guide_benchmark);
        CHECK(!cfg.allow_cross_bucket_naming);
    }

    // No bucket flag: the sweep is the normal bucket, but a filter naming a test exactly may cross buckets
    // (decided per-test in is_eligible, not here). run_disabled_tests is never auto-set — an exact name is
    // what enables a disabled test, so a substring filter can't resurrect an unrelated one.
    {
        char a0[] = "prog";
        char a1[] = "T_manual_bench";
        char* argv[] = {a0, a1};
        auto const cfg = nx::test_schedule_config::create_from_args(2, argv);
        CHECK(cfg.selected_bucket == nx::config::test_bucket::normal);
        CHECK(cfg.allow_cross_bucket_naming);
        CHECK(!cfg.run_disabled_tests);
    }

    // A filter with an explicit bucket flag stays restricted to that bucket, and still does not flip
    // run_disabled_tests.
    {
        char a0[] = "prog";
        char a1[] = "--manual";
        char a2[] = "T_manual_bench";
        char* argv[] = {a0, a1, a2};
        auto const cfg = nx::test_schedule_config::create_from_args(3, argv);
        CHECK(cfg.selected_bucket == nx::config::test_bucket::manual);
        CHECK(!cfg.allow_cross_bucket_naming);
        CHECK(!cfg.run_disabled_tests);
    }
}

TEST("test schedule config - a substring filter never reaches another bucket")
{
    // The reported bug: `dev.py test "async"` ran not just the normal async tests but every manual and
    // guide-benchmark test whose name merely contained "async" — expensive benchmarks in an unattended run.
    // Drives the real CLI path (create_from_args), which is where the gate used to open up globally.
    nx::test_registry reg;

    int counter_normal = 0;
    int counter_manual = 0;
    int counter_guide = 0;

    reg.add_declaration("async - queue", {},
                        [&]
                        {
                            ++counter_normal;
                            SUCCEED();
                        });
    reg.add_declaration("async - throughput bench", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [&] { ++counter_manual; });
    reg.add_declaration("async - guide bench", nx::config::cfg{.bucket = nx::config::test_bucket::guide_benchmark},
                        [&] { ++counter_guide; });

    auto const config_from = [](cc::span<char const* const> args)
    { return nx::test_schedule_config::create_from_args(int(args.size()), const_cast<char**>(args.data())); };

    // A bare substring filter: only the normal-bucket test, though "async" matches all three names.
    {
        char const* const args[] = {"prog", "async"};
        auto exec = nx::execute_tests(nx::test_schedule::create(config_from(args), reg), {});
        CHECK(counter_normal == 1);
        CHECK(counter_manual == 0);
        CHECK(counter_guide == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // An exact name reaches the manual test.
    counter_normal = counter_manual = counter_guide = 0;
    {
        char const* const args[] = {"prog", "async - throughput bench"};
        auto exec = nx::execute_tests(nx::test_schedule::create(config_from(args), reg), {});
        CHECK(counter_normal == 0);
        CHECK(counter_manual == 1);
        CHECK(exec.count_total_tests() == 1);
    }

    // So does the enabling flag, and it sweeps by substring within its own bucket.
    counter_normal = counter_manual = counter_guide = 0;
    {
        char const* const args[] = {"prog", "--manual", "async"};
        auto exec = nx::execute_tests(nx::test_schedule::create(config_from(args), reg), {});
        CHECK(counter_normal == 0);
        CHECK(counter_manual == 1);
        CHECK(counter_guide == 0);
        CHECK(exec.count_total_tests() == 1);
    }

    // --guide-benchmarks likewise (the path dev.py pgo drives).
    counter_normal = counter_manual = counter_guide = 0;
    {
        char const* const args[] = {"prog", "--guide-benchmarks", "async"};
        auto exec = nx::execute_tests(nx::test_schedule::create(config_from(args), reg), {});
        CHECK(counter_guide == 1);
        CHECK(counter_manual == 0);
        CHECK(exec.count_total_tests() == 1);
    }
}
