#include <clean-core/string/format.hh>
#include <clean-core/string/to_debug_string.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <stdexcept>
#include <string>

TEST("check - basic CHECK passes on true expression")
{
    nx::test_registry reg;
    reg.add_declaration("check_pass", {},
                        []
                        {
                            CHECK(true);
                            CHECK(1 == 1);
                            CHECK(5 > 3);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - basic CHECK fails on false expression")
{
    nx::test_registry reg;
    reg.add_declaration("check_fail", {},
                        []
                        {
                            CHECK(false);
                            CHECK(1 == 2);
                            CHECK(3 > 5);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 3);
}

TEST("check - CHECK continues execution after failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("check_continues", {},
                        [&counter]
                        {
                            CHECK(false);
                            counter++;
                            CHECK(true);
                            counter++;
                            CHECK(1 == 2);
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 3);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - REQUIRE passes on true expression")
{
    nx::test_registry reg;
    reg.add_declaration("require_pass", {},
                        []
                        {
                            REQUIRE(true);
                            REQUIRE(1 == 1);
                            REQUIRE(5 > 3);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - REQUIRE aborts execution on failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_aborts", {},
                        [&counter]
                        {
                            REQUIRE(false);
                            counter++;   // should NOT execute
                            CHECK(true); // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - SUCCEED always passes")
{
    nx::test_registry reg;
    reg.add_declaration("succeed_test", {},
                        []
                        {
                            SUCCEED();
                            SUCCEED("with message");
                            SUCCEED("another success");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - FAIL always fails and aborts")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("fail_test", {},
                        [&counter]
                        {
                            FAIL();
                            counter++; // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("check - FAIL with message")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("fail_msg_test", {},
                        [&counter]
                        {
                            FAIL("custom failure message");
                            counter++; // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("check - comparison operators lt lte gt gte")
{
    nx::test_registry reg;
    reg.add_declaration("comparison_ops", {},
                        []
                        {
                            CHECK(1 < 2);
                            CHECK(2 <= 2);
                            CHECK(3 <= 5);
                            CHECK(5 > 3);
                            CHECK(4 >= 4);
                            CHECK(7 >= 3);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 6);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - equality operators == !=")
{
    nx::test_registry reg;
    reg.add_declaration("equality_ops", {},
                        []
                        {
                            CHECK(5 == 5);
                            CHECK(3 != 7);
                            CHECK("hello" != "world");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - mixed CHECK and REQUIRE")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("mixed_checks", {},
                        [&counter]
                        {
                            CHECK(true);
                            counter++;
                            REQUIRE(true);
                            counter++;
                            CHECK(false); // fails but continues
                            counter++;
                            REQUIRE(false); // fails and aborts
                            counter++;      // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 3);
    CHECK(exec.count_total_checks() == 4);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - chaining with .context()")
{
    nx::test_registry reg;
    reg.add_declaration("context_chain", {},
                        []
                        {
                            CHECK(1 == 1).context("basic equality");
                            CHECK(2 > 1).context("comparison check");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - chaining with .note()")
{
    nx::test_registry reg;
    reg.add_declaration("note_chain", {},
                        []
                        {
                            CHECK(true).note("first check");
                            CHECK(1 != 2).note("inequality check");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - chaining with .dump()")
{
    nx::test_registry reg;
    reg.add_declaration("dump_chain", {},
                        []
                        {
                            int value = 42;
                            CHECK(value == 42).dump("value", value);
                            CHECK(value != 0).dump(value);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - multiple chaining")
{
    nx::test_registry reg;
    reg.add_declaration("multi_chain", {},
                        []
                        {
                            int x = 5;
                            CHECK(x > 0).context("validation").note("checking positivity").dump("x", x);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

namespace
{
// Custom type to track when to_debug_string is called
struct call_tracker
{
    int value;
    mutable bool to_string_called = false;

    explicit call_tracker(int v) : value(v) {}

    bool operator==(call_tracker const& other) const { return value == other.value; }

    bool operator!=(call_tracker const& other) const { return value != other.value; }

    bool operator<(call_tracker const& other) const { return value < other.value; }

    cc::string to_string() const
    {
        to_string_called = true;
        return cc::format("call_tracker({})", value);
    }
};
} // namespace

TEST("check - to_debug_string only called on failing checks")
{
    nx::test_registry reg;

    call_tracker pass_lhs{10};
    call_tracker pass_rhs{10};
    call_tracker fail_lhs{5};
    call_tracker fail_rhs{7};

    reg.add_declaration("debug_string_test", {},
                        [&]
                        {
                            // Passing check - to_debug_string should NOT be called
                            CHECK(pass_lhs == pass_rhs);

                            // Failing check - to_debug_string SHOULD be called
                            CHECK(fail_lhs == fail_rhs);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify passing check did not call to_debug_string
    CHECK(pass_lhs.to_string_called == false);
    CHECK(pass_rhs.to_string_called == false);

    // Verify failing check did call to_debug_string
    CHECK(fail_lhs.to_string_called == true);
    CHECK(fail_rhs.to_string_called == true);

    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - to_debug_string only called on failing REQUIRE")
{
    nx::test_registry reg;

    call_tracker pass_lhs{10};
    call_tracker pass_rhs{10};
    call_tracker fail_lhs{5};
    call_tracker fail_rhs{7};

    int counter = 0;

    reg.add_declaration("debug_string_require_test", {},
                        [&]
                        {
                            // Passing REQUIRE - to_debug_string should NOT be called
                            REQUIRE(pass_lhs == pass_rhs);
                            counter++;

                            // Failing REQUIRE - to_debug_string SHOULD be called
                            REQUIRE(fail_lhs == fail_rhs);
                            counter++; // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify passing REQUIRE did not call to_debug_string
    CHECK(pass_lhs.to_string_called == false);
    CHECK(pass_rhs.to_string_called == false);

    // Verify failing REQUIRE did call to_debug_string
    CHECK(fail_lhs.to_string_called == true);
    CHECK(fail_rhs.to_string_called == true);

    // Verify REQUIRE aborted
    CHECK(counter == 1);

    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - to_debug_string with comparison operators")
{
    nx::test_registry reg;

    call_tracker pass_less{3};
    call_tracker pass_less_rhs{5};
    call_tracker fail_less{7};
    call_tracker fail_less_rhs{2};

    reg.add_declaration("debug_string_comparison_test", {},
                        [&]
                        {
                            // Passing comparison - to_debug_string should NOT be called
                            CHECK(pass_less < pass_less_rhs);

                            // Failing comparison - to_debug_string SHOULD be called
                            CHECK(fail_less < fail_less_rhs);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Verify passing check did not call to_debug_string
    CHECK(pass_less.to_string_called == false);
    CHECK(pass_less_rhs.to_string_called == false);

    // Verify failing check did call to_debug_string
    CHECK(fail_less.to_string_called == true);
    CHECK(fail_less_rhs.to_string_called == true);

    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - boolean expression CHECK")
{
    nx::test_registry reg;
    reg.add_declaration("bool_expr_test", {},
                        []
                        {
                            bool flag = true;
                            CHECK(flag);

                            int* ptr = nullptr;
                            CHECK(!ptr);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - string comparison")
{
    nx::test_registry reg;
    reg.add_declaration("string_compare_test", {},
                        []
                        {
                            std::string a = "hello";
                            std::string b = "hello";
                            std::string c = "world";

                            CHECK(a == b);
                            CHECK(a != c);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - pointer comparison")
{
    nx::test_registry reg;
    reg.add_declaration("pointer_compare_test", {},
                        []
                        {
                            int value = 42;
                            int* ptr = &value;
                            int* null_ptr = nullptr;

                            CHECK(ptr != nullptr);
                            CHECK(null_ptr == nullptr);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - floating point comparison")
{
    nx::test_registry reg;
    reg.add_declaration("float_compare_test", {},
                        []
                        {
                            double a = 1.5;
                            double b = 1.5;
                            double c = 2.5;

                            CHECK(a == b);
                            CHECK(a < c);
                            CHECK(c > a);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - SUCCEED allows test to pass explicitly")
{
    nx::test_registry reg;

    int branch = 2;

    reg.add_declaration("succeed_branch_test", {},
                        [&]
                        {
                            if (branch == 1)
                            {
                                CHECK(false);
                            }
                            else if (branch == 2)
                            {
                                SUCCEED("took correct branch");
                            }
                            else
                            {
                                FAIL("unexpected branch");
                            }
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - SKIP aborts test execution")
{
    nx::test_registry reg;

    bool executed = false;

    reg.add_declaration("skip_test", {},
                        [&]
                        {
                            SKIP("not ready");
                            executed = true;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(executed == false);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - complex expression with multiple operators")
{
    nx::test_registry reg;
    reg.add_declaration("complex_expr_test", {},
                        []
                        {
                            int a = 5;
                            int b = 10;
                            int c = 15;

                            CHECK(a < b);
                            CHECK(b < c);
                            CHECK(a + b == c);
                            CHECK(c - b == a);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 4);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - CHECK_THROWS passes when expression throws")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_pass", {},
                        []
                        {
                            CHECK_THROWS(throw std::runtime_error("expected"));
                            CHECK_THROWS(throw 42);
                            CHECK_THROWS(throw "error");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - CHECK_THROWS fails when expression does not throw")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_fail", {},
                        []
                        {
                            CHECK_THROWS(42);
                            CHECK_THROWS(1 + 2);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - CHECK_THROWS continues execution after failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("check_throws_continues", {},
                        [&counter]
                        {
                            CHECK_THROWS(1 + 2); // fails, no throw
                            counter++;
                            CHECK_THROWS(throw 42); // passes
                            counter++;
                            CHECK_THROWS(counter++); // fails, no throw
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 4);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - CHECK_THROWS_AS passes when correct exception type is thrown")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_as_pass", {},
                        []
                        {
                            CHECK_THROWS_AS(throw std::runtime_error("test"), std::runtime_error);
                            CHECK_THROWS_AS(throw std::logic_error("test"), std::logic_error);
                            CHECK_THROWS_AS(throw std::exception(), std::exception);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - CHECK_THROWS_AS fails when expression does not throw")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_as_no_throw", {},
                        []
                        {
                            CHECK_THROWS_AS(42, std::runtime_error);
                            CHECK_THROWS_AS(1 + 2, std::logic_error);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - CHECK_THROWS_AS fails when wrong exception type is thrown")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_as_wrong_type", {},
                        []
                        {
                            CHECK_THROWS_AS(throw std::runtime_error("test"), std::logic_error);
                            CHECK_THROWS_AS(throw 42, std::runtime_error);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - CHECK_THROWS_AS continues execution after failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("check_throws_as_continues", {},
                        [&counter]
                        {
                            CHECK_THROWS_AS(1 + 2, std::runtime_error); // fails, no throw
                            counter++;
                            CHECK_THROWS_AS(throw std::runtime_error("ok"), std::runtime_error); // passes
                            counter++;
                            CHECK_THROWS_AS(throw std::logic_error("wrong"), std::runtime_error); // fails, wrong type
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 3);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - REQUIRE_THROWS passes when expression throws")
{
    nx::test_registry reg;
    reg.add_declaration("require_throws_pass", {},
                        []
                        {
                            REQUIRE_THROWS(throw std::runtime_error("expected"));
                            REQUIRE_THROWS(throw 42);
                            REQUIRE_THROWS(throw "error");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - REQUIRE_THROWS aborts execution when expression does not throw")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_throws_aborts", {},
                        [&counter]
                        {
                            REQUIRE_THROWS(1 + 2); // fails, no throw - aborts
                            counter++;             // should NOT execute
                            CHECK(true);           // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - REQUIRE_THROWS_AS passes when correct exception type is thrown")
{
    nx::test_registry reg;
    reg.add_declaration("require_throws_as_pass", {},
                        []
                        {
                            REQUIRE_THROWS_AS(throw std::runtime_error("test"), std::runtime_error);
                            REQUIRE_THROWS_AS(throw std::logic_error("test"), std::logic_error);
                            REQUIRE_THROWS_AS(throw std::exception(), std::exception);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - REQUIRE_THROWS_AS aborts when expression does not throw")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_throws_as_no_throw_aborts", {},
                        [&counter]
                        {
                            REQUIRE_THROWS_AS(1 + 2, std::runtime_error); // fails, no throw - aborts
                            counter++;                                    // should NOT execute
                            CHECK(true);                                  // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - REQUIRE_THROWS_AS aborts when wrong exception type is thrown")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_throws_as_wrong_type_aborts", {},
                        [&counter]
                        {
                            REQUIRE_THROWS_AS(throw std::logic_error("wrong"),
                                              std::runtime_error); // fails, wrong type - aborts
                            counter++;                             // should NOT execute
                            CHECK(true);                           // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - CHECK_THROWS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_chain", {},
                        []
                        {
                            CHECK_THROWS(throw std::runtime_error("test")).context("error handling");
                            CHECK_THROWS(throw 42).note("integer exception");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - CHECK_THROWS_AS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration("check_throws_as_chain", {},
                        []
                        {
                            CHECK_THROWS_AS(throw std::runtime_error("test"), std::runtime_error).context("validation");
                            CHECK_THROWS_AS(throw std::logic_error("test"), std::logic_error).note("setup phase");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - REQUIRE_THROWS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration("require_throws_chain", {},
                        []
                        {
                            REQUIRE_THROWS(throw std::runtime_error("test")).context("precondition check");
                            REQUIRE_THROWS(throw 42).note("must throw");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - REQUIRE_THROWS_AS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration(
        "require_throws_as_chain", {},
        []
        {
            REQUIRE_THROWS_AS(throw std::runtime_error("test"), std::runtime_error).context("setup phase");
            REQUIRE_THROWS_AS(throw std::logic_error("test"), std::logic_error).note("validation");
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - mixed throw checks and regular checks")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("mixed_throw_checks", {},
                        [&counter]
                        {
                            CHECK(true);
                            counter++;
                            CHECK_THROWS(throw 42);
                            counter++;
                            CHECK_THROWS_AS(throw std::runtime_error("test"), std::runtime_error);
                            counter++;
                            REQUIRE_THROWS(throw "error");
                            counter++;
                            REQUIRE_THROWS_AS(throw std::logic_error("test"), std::logic_error);
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 5);
    CHECK(exec.count_total_checks() == 5);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - throw checks with exception inheritance")
{
    nx::test_registry reg;
    reg.add_declaration("throw_checks_inheritance", {},
                        []
                        {
                            // std::runtime_error derives from std::exception
                            CHECK_THROWS_AS(throw std::runtime_error("test"), std::exception);

                            // std::logic_error derives from std::exception
                            REQUIRE_THROWS_AS(throw std::logic_error("test"), std::exception);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

#if CC_ASSERT_ENABLED

namespace
{
// Helper functions that should trigger assertions
void divide_by_zero(int denominator)
{
    CC_ASSERT(denominator != 0, "cannot divide by zero");
    int const result = 42 / denominator;
    (void)result;
}

void validate_positive(int value)
{
    CC_ASSERT(value > 0, "value must be positive");
}

void check_range(int value, int min, int max)
{
    CC_ASSERT(value >= min && value <= max, "value out of range");
}

void no_assertion_function(int value)
{
    // This function does not trigger any assertions
    (void)value;
}
} // namespace

TEST("check - CHECK_ASSERTS passes when assertion is triggered")
{
    nx::test_registry reg;
    reg.add_declaration("check_asserts_pass", {},
                        []
                        {
                            CHECK_ASSERTS(divide_by_zero(0));
                            CHECK_ASSERTS(validate_positive(-5));
                            CHECK_ASSERTS(check_range(100, 0, 10));
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - CHECK_ASSERTS fails when no assertion is triggered")
{
    nx::test_registry reg;
    reg.add_declaration("check_asserts_fail", {},
                        []
                        {
                            CHECK_ASSERTS(divide_by_zero(5));
                            CHECK_ASSERTS(validate_positive(10));
                            CHECK_ASSERTS(no_assertion_function(42));
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 3);
}

TEST("check - CHECK_ASSERTS continues execution after failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("check_asserts_continues", {},
                        [&counter]
                        {
                            CHECK_ASSERTS(no_assertion_function(1)); // fails, no assertion
                            counter++;
                            CHECK_ASSERTS(divide_by_zero(0)); // passes
                            counter++;
                            CHECK_ASSERTS(validate_positive(5)); // fails, no assertion
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 3);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - REQUIRE_ASSERTS passes when assertion is triggered")
{
    nx::test_registry reg;
    reg.add_declaration("require_asserts_pass", {},
                        []
                        {
                            REQUIRE_ASSERTS(divide_by_zero(0));
                            REQUIRE_ASSERTS(validate_positive(-1));
                            REQUIRE_ASSERTS(check_range(50, 0, 10));
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - REQUIRE_ASSERTS aborts execution when no assertion is triggered")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_asserts_aborts", {},
                        [&counter]
                        {
                            REQUIRE_ASSERTS(no_assertion_function(42)); // fails, no assertion - aborts
                            counter++;                                  // should NOT execute
                            CHECK(true);                                // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 0);
    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("check - CHECK_ASSERTS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration("check_asserts_chain", {},
                        []
                        {
                            CHECK_ASSERTS(divide_by_zero(0)).context("division test");
                            CHECK_ASSERTS(validate_positive(-10)).note("validation check");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - REQUIRE_ASSERTS with chaining")
{
    nx::test_registry reg;
    reg.add_declaration("require_asserts_chain", {},
                        []
                        {
                            REQUIRE_ASSERTS(divide_by_zero(0)).context("precondition check");
                            REQUIRE_ASSERTS(check_range(100, 1, 10)).note("boundary validation");
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - mixed assert checks and regular checks")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("mixed_assert_checks", {},
                        [&counter]
                        {
                            CHECK(true);
                            counter++;
                            CHECK_ASSERTS(divide_by_zero(0));
                            counter++;
                            CHECK(1 == 1);
                            counter++;
                            REQUIRE_ASSERTS(validate_positive(-5));
                            counter++;
                            CHECK(counter == 4);
                            counter++;
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 5);
    CHECK(exec.count_total_checks() == 5);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("check - mixed assert checks with failures")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("mixed_assert_checks_fail", {},
                        [&counter]
                        {
                            CHECK(true);
                            counter++;
                            CHECK_ASSERTS(no_assertion_function(1)); // fails, no assertion
                            counter++;
                            CHECK_ASSERTS(divide_by_zero(0)); // passes
                            counter++;
                            REQUIRE_ASSERTS(no_assertion_function(2)); // fails, no assertion - aborts
                            counter++;                                 // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 3);
    CHECK(exec.count_total_checks() == 4);
    CHECK(exec.count_failed_checks() == 2);
}

TEST("check - CHECK_ASSERTS with lambda")
{
    nx::test_registry reg;
    reg.add_declaration("check_asserts_lambda", {},
                        []
                        {
                            int const x = 0;
                            CHECK_ASSERTS([&]() { CC_ASSERT(x > 0, "x must be positive"); }());

                            int const y = 10;
                            CHECK_ASSERTS([&]() { CC_ASSERT(y < 5, "y must be less than 5"); }());
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("check - REQUIRE_ASSERTS aborts on first failure")
{
    nx::test_registry reg;

    int counter = 0;
    reg.add_declaration("require_asserts_aborts_first", {},
                        [&counter]
                        {
                            REQUIRE_ASSERTS(divide_by_zero(0)); // passes
                            counter++;
                            REQUIRE_ASSERTS(validate_positive(10)); // fails - aborts
                            counter++;                              // should NOT execute
                            REQUIRE_ASSERTS(divide_by_zero(0));     // should NOT execute
                            counter++;                              // should NOT execute
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_checks() == 1);
}

#endif // CC_ASSERT_ENABLED
