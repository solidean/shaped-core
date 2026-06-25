#include <clean-core/string/string.hh>
#include <nexus/fuzz/fuzz.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <stdexcept>

// Exercises the recommended SECTION workflow: setup in the outer TEST, a SECTION to fuzz, and a
// SECTION pinning a known behavior. This invariant always holds, so the fuzz SECTION passes.
TEST("fuzz engine - section workflow")
{
    auto test = nx::fuzz::test::create();
    test->add_value("seed", 0);
    test->add_op("inc", [](int a) { return a + 1; });
    test->add_invariant("non-negative", [](int i) { return i >= 0; });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }

    SECTION("inc adds one")
    {
        CHECK(test->eval_op_to<int>("inc", 41) == 42);
    }
}

TEST("fuzz engine - direct eval of operations")
{
    auto t = nx::fuzz::test::create();
    t->add_value("1", 1);
    t->add_value("3", 3);
    t->add_op("add", [](int a, int b) { return a + b; });
    t->add_op("sub", [](int a, int b) { return a - b; });

    CHECK(t->eval_op_to<int>("add", 1, 3) == 4);
    CHECK(t->eval_op_to<int>("sub", 3, 1) == 2);

    // chaining: a result feeds the next operation
    auto r = t->eval_op("add", 1, 3);
    CHECK(t->eval_op_to<int>("add", r, r) == 8);
}

TEST("fuzz engine - detects a thrown exception")
{
    auto t = nx::fuzz::test::create();
    t->add_value("x", 0);
    t->add_op("boom", [](int) { throw std::runtime_error("boom"); });

    auto res = t->execute_fuzzer(1);
    CHECK(!res.is_ok);
    REQUIRE(res.failing_run.has_value());
}

TEST("fuzz engine - detects a false invariant")
{
    auto t = nx::fuzz::test::create();
    t->add_value("x", 5);
    t->add_invariant("never", [](int) { return false; });

    auto res = t->execute_fuzzer(1);
    CHECK(!res.is_ok);
    REQUIRE(res.failing_run.has_value());
}

TEST("fuzz engine - detects a failed CHECK inside an operation")
{
    auto t = nx::fuzz::test::create();
    t->add_value("x", 5);
    t->add_invariant("must-be-small", [](int i) { CHECK(i < 0); });

    auto res = t->execute_fuzzer(1);
    CHECK(!res.is_ok);
    REQUIRE(res.failing_run.has_value());
}

TEST("fuzz engine - uncreatable argument type is a setup error, not a finding")
{
    auto t = nx::fuzz::test::create();
    t->add_op("needs-string", [](cc::string) {});

    auto res = t->execute_fuzzer(1);
    CHECK(!res.is_ok);
    CHECK(!res.failing_run.has_value()); // setup error: no finding
}

TEST("fuzz engine - a passing test reports no failures and does not pollute the host")
{
    // run an inner fuzz test that never fails inside its own execution context
    nx::test_registry reg;
    reg.add_declaration("inner-pass", {},
                        []
                        {
                            auto t = nx::fuzz::test::create();
                            t->add_value("x", 1);
                            t->add_op("inc", [](int a) { return a + 1; });
                            t->add_invariant("always", [](int) { return true; });
                            CHECK(t->execute_fuzz_test());
                        });

    auto sched = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(sched, {});
    CHECK(exec.count_failed_checks() == 0);
}

TEST("fuzz engine - a failing test reports exactly one failed check (no pollution)")
{
    // The many CHECK/invariant failures probed during fuzzing + minimization must NOT leak into the
    // host test: only the single outer CHECK(execute_fuzz_test()) should be recorded as failing.
    nx::test_registry reg;
    reg.add_declaration("inner-fail", {},
                        []
                        {
                            auto t = nx::fuzz::test::create();
                            t->add_value("3", 3);
                            t->add_op("add1", [](int a) { return a + 1; });
                            t->add_invariant("is-not-7", [](int i) { return i != 7; });
                            CHECK(t->execute_fuzz_test());
                        });

    auto sched = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(sched, {});
    CHECK(exec.count_failed_checks() == 1);
}
