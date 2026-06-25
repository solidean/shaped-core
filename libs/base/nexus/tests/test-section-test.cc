#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST("test sections - basics")
{
    int counter_a = 0;
    int counter_b = 0;
    int counter_c = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testA", {},
        [&]
        {
            counter_a++;

            SECTION("sec A")
            {
                counter_b++;
                SUCCEED();
            }

            SECTION("sec B")
            {
                counter_c++;
                SUCCEED();
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_a == 2);
    CHECK(counter_b == 1);
    CHECK(counter_c == 1);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - basics nested")
{
    int counter_a = 0;
    int counter_b = 0;
    int counter_c = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testA", {},
        [&]
        {
            SECTION("outer")
            {
                counter_a++;

                SECTION("sec A")
                {
                    counter_b++;
                    SUCCEED();
                }

                SECTION("sec B")
                {
                    counter_c++;
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_a == 2);
    CHECK(counter_b == 1);
    CHECK(counter_c == 1);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - canonical preorder + counts on a richer tree")
{
    int counter_root = 0;
    int counter_a = 0;
    int counter_a1 = 0;
    int counter_a2 = 0;
    int counter_b = 0;
    int counter_b1 = 0;
    int counter_c = 0;
    std::vector<int> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testMultiLevel", {},
        [&]
        {
            counter_root++;

            SECTION("A")
            {
                counter_a++;
                log.push_back(1);

                SECTION("A1")
                {
                    counter_a1++;
                    log.push_back(2);
                    SUCCEED();
                }

                SECTION("A2")
                {
                    counter_a2++;
                    log.push_back(3);
                    SUCCEED();
                }
            }

            SECTION("B")
            {
                counter_b++;
                log.push_back(4);

                SECTION("B1")
                {
                    counter_b1++;
                    log.push_back(5);
                    SUCCEED();
                }
            }

            SECTION("C")
            {
                counter_c++;
                log.push_back(6);
                SUCCEED();
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_root == 4);
    CHECK(counter_a == 2);
    CHECK(counter_a1 == 1);
    CHECK(counter_a2 == 1);
    CHECK(counter_b == 1);
    CHECK(counter_b1 == 1);
    CHECK(counter_c == 1);

    // Each run executes from root to leaf, so log is interleaved:
    // Run 1: root -> A -> A1: logs 1, 2
    // Run 2: root -> A -> A2: logs 1, 3
    // Run 3: root -> B -> B1: logs 4, 5
    // Run 4: root -> C: logs 6
    auto const expected_log = std::vector<int>{1, 2, 1, 3, 4, 5, 6};
    CHECK(log == expected_log);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 4);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - distinct dynamic sections in a loop, preorder stable")
{
    int n = 4;
    int counter_root = 0;
    std::vector<int> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testDynamicLoop", {},
        [&]
        {
            counter_root++;

            for (int i = 0; i < n; ++i)
            {
                SECTION("item {}", i)
                {
                    log.push_back(i);
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_root == n);

    auto const expected_log = std::vector<int>{0, 1, 2, 3};
    CHECK(log == expected_log);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == n);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - dynamic loop with nested subsections, preorder and counts")
{
    int n = 3;
    int counter_root = 0;
    std::vector<std::pair<int, std::string>> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testDynamicNested", {},
        [&]
        {
            counter_root++;

            for (int i = 0; i < n; ++i)
            {
                SECTION("i={}", i)
                {
                    SECTION("even")
                    {
                        log.emplace_back(i, "even");
                        SUCCEED();
                    }

                    SECTION("odd")
                    {
                        log.emplace_back(i, "odd");
                        SUCCEED();
                    }
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_root == 2 * n);

    // Each run executes from root through parent section to leaf:
    // Run for i=0/even, then i=0/odd, then i=1/even, then i=1/odd, etc.
    auto const expected = std::vector<std::pair<int, std::string>>{
        {0, "even"}, {0, "odd"}, {1, "even"}, {1, "odd"}, {2, "even"}, {2, "odd"},
    };
    CHECK(log == expected);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2 * n);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - conditionally active subsections across runs (all reachable)")
{
    int counter_root = 0;
    std::vector<std::string> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testConditional", {},
        [&, phase = 0]() mutable
        {
            counter_root++;

            SECTION("outer")
            {
                if (phase == 0)
                {
                    SECTION("first")
                    {
                        log.emplace_back("first");
                        phase = 1;
                        SUCCEED();
                    }
                }
                else
                {
                    SECTION("second")
                    {
                        log.emplace_back("second");
                        SUCCEED();
                    }
                }

                // NOTE: we need another section here so we don't close outer early
                //       this "ultra" dynamic discovery is not super useful
                SECTION("third")
                {
                    log.emplace_back("third");
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_root == 3);

    auto const expected = std::vector<std::string>{
        "first",
        "second",
        "third",
    };
    CHECK(log == expected);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - discovered-but-vanishing subsection => failure but no infinite loop")
{
    int visits_outer = 0;
    int visits_once = 0;
    int visits_vanish = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testVanishing", {},
        [&, flag = false]() mutable
        {
            SECTION("outer")
            {
                ++visits_outer;

                if (!flag)
                {
                    SECTION("once")
                    {
                        ++visits_once;
                        flag = true;
                        SUCCEED();
                    }

                    SECTION("vanish")
                    {
                        ++visits_vanish;
                        SUCCEED();
                    }
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(visits_outer == 2);
    CHECK(visits_once == 1);
    CHECK(visits_vanish == 0);

    CHECK(exec.count_failed_tests() == 1);
}

TEST("test sections - duplicate sibling section names => immediate error")
{
    int first = 0;
    int second = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testDuplicate", {},
        [&]
        {
            SECTION("parent")
            {
                SECTION("dup")
                {
                    ++first;
                    SUCCEED();
                }

                SECTION("dup")
                {
                    ++second;
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(first == 1);
    CHECK(second == 0);

    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 1);
}

TEST("test sections - failure in one leaf still allows siblings to run")
{
    std::vector<std::string> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testNonFatalFailure", {},
        [&]
        {
            SECTION("Root")
            {
                SECTION("A")
                {
                    log.emplace_back("A");
                    CHECK(false);
                }

                SECTION("B")
                {
                    log.emplace_back("B");
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    auto const expected = std::vector<std::string>{"A", "B"};
    CHECK(log == expected);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
}

TEST("test sections - exception inside one leaf doesn't corrupt scheduling")
{
    std::vector<std::string> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testException", {},
        [&]
        {
            SECTION("Root")
            {
                SECTION("throws")
                {
                    log.emplace_back("throws_enter");
                    throw std::runtime_error("test exception");
                }

                SECTION("ok")
                {
                    log.emplace_back("ok_enter");
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    auto const expected = std::vector<std::string>{
        "throws_enter",
        // "ok_enter", -> for now, siblings are not guaranteed to be visited
    };
    CHECK(log == expected);

    CHECK(exec.count_failed_tests() == 1);
}

TEST("test sections - early-abort-style macro (REQUIRE) terminates path but not schedule")
{
    int after_require = 0;
    int visited_ok = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testRequire", {},
        [&]
        {
            SECTION("Root")
            {
                SECTION("fatal")
                {
                    REQUIRE(false);
                    ++after_require;
                }

                SECTION("ok")
                {
                    ++visited_ok;
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(after_require == 0);
    CHECK(visited_ok == 0); // NOTE: for now, siblings might be visited

    CHECK(exec.count_failed_tests() == 1);
}

TEST("test sections - nested conditionals with subsections active at different times")
{
    int n = 2;
    int counter_root = 0;
    std::vector<std::string> log;

    nx::test_registry reg;
    reg.add_declaration( //
        "testNestedConditional", {},
        [&]
        {
            counter_root++;

            for (int i = 0; i < n; ++i)
            {
                SECTION("i={}", i)
                {
                    if (i == 0)
                    {
                        SECTION("X")
                        {
                            log.emplace_back("i0/X");
                            SUCCEED();
                        }
                    }
                    else
                    {
                        SECTION("Y")
                        {
                            log.emplace_back("i1/Y");
                            SUCCEED();
                        }
                    }
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(counter_root == 2);

    auto const expected = std::vector<std::string>{
        "i0/X",
        "i1/Y",
    };
    CHECK(log == expected);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);
}

TEST("test sections - leaf sections with no checks are considered failing")
{
    int visited_empty_leaf = 0;
    int visited_valid_leaf = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testNoChecks", {},
        [&]
        {
            SECTION("parent")
            {
                SECTION("empty leaf")
                {
                    ++visited_empty_leaf;
                    // No CHECK or REQUIRE calls here - should cause failure
                }

                SECTION("valid leaf")
                {
                    ++visited_valid_leaf;
                    SUCCEED();
                }
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(visited_empty_leaf == 1);
    CHECK(visited_valid_leaf == 1);

    // The test should be considered failed because one leaf section has no checks
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
}

TEST("test sections - CC_ASSERT_ALWAYS failure in root after subsections executes on all paths")
{
    int visited_a = 0;
    int visited_b = 0;
    int after_subsections = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testAssertAlwaysInRoot", {},
        [&]
        {
            SECTION("parent")
            {
                SECTION("A")
                {
                    ++visited_a;
                    SUCCEED();
                }

                SECTION("B")
                {
                    ++visited_b;
                    SUCCEED();
                }

                // This is in the root section (parent) but after the subsections
                // It should execute twice - once during the run for section A, and once for section B
                ++after_subsections;
                CC_ASSERT_ALWAYS(false, "deliberate assert failure after subsections");
            }
        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // Both subsections should be visited exactly once
    CHECK(visited_a == 1);
    CHECK(visited_b == 1);

    // The assert failure should have been hit twice (once per subsection path)
    CHECK(after_subsections == 2);

    // Both checks should have been executed successfully
    // (assertion failures count as failed checks)
    CHECK(exec.count_total_checks() == 4);
    CHECK(exec.count_failed_checks() == 2);

    // But the test overall should be marked as failed due to the CC_ASSERT_ALWAYS failures
    CHECK(exec.count_failed_tests() == 1);
}

TEST("test sections - filter single top-level section")
{
    int counter_a = 0;
    int counter_b = 0;
    int counter_c = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testFilter", {},
        [&]
        {
            SECTION("sa")
            {
                counter_a++;
                SUCCEED();
            }

            SECTION("sb")
            {
                counter_b++;
                SUCCEED();
            }

            SECTION("sc")
            {
                counter_c++;
                SUCCEED();
            }
        });

    nx::test_schedule_config config;
    config.section_filters = {"sb"};

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    CHECK(counter_a == 0);
    CHECK(counter_b == 1);
    CHECK(counter_c == 0);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test sections - filter nested section path")
{
    int counter_outer = 0;
    int counter_sa = 0;
    int counter_sb = 0;
    int counter_sc = 0;
    int counter_sd = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testFilterNested", {},
        [&]
        {
            SECTION("outer")
            {
                counter_outer++;

                SECTION("sa")
                {
                    counter_sa++;

                    SECTION("sb")
                    {
                        counter_sb++;
                        SUCCEED();
                    }

                    SECTION("sc")
                    {
                        counter_sc++;
                        SUCCEED();
                    }
                }

                SECTION("sd")
                {
                    counter_sd++;
                    SUCCEED();
                }
            }
        });

    nx::test_schedule_config config;
    config.section_filters = {"outer", "sa", "sb"};

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    // outer and sa should be entered once to reach sb
    CHECK(counter_outer == 1);
    CHECK(counter_sa == 1);
    CHECK(counter_sb == 1);
    // sc and sd should not be entered
    CHECK(counter_sc == 0);
    CHECK(counter_sd == 0);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 1);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test sections - filter parent section runs all children")
{
    int counter_sa = 0;
    int counter_sb = 0;
    int counter_sc = 0;
    int counter_sd = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testFilterParent", {},
        [&]
        {
            SECTION("sa")
            {
                counter_sa++;

                SECTION("sb")
                {
                    counter_sb++;
                    SUCCEED();
                }

                SECTION("sc")
                {
                    counter_sc++;
                    SUCCEED();
                }
            }

            SECTION("sd")
            {
                counter_sd++;
                SUCCEED();
            }
        });

    nx::test_schedule_config config;
    config.section_filters = {"sa"};

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    // sa should be entered twice (once for sb, once for sc)
    CHECK(counter_sa == 2);
    CHECK(counter_sb == 1);
    CHECK(counter_sc == 1);
    // sd should not be entered
    CHECK(counter_sd == 0);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test sections - filter with deeper nesting")
{
    int counter_a = 0;
    int counter_b = 0;
    int counter_c = 0;
    int counter_d = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testFilterDeep", {},
        [&]
        {
            SECTION("a")
            {
                counter_a++;

                SECTION("b")
                {
                    counter_b++;

                    SECTION("c")
                    {
                        counter_c++;
                        SUCCEED();
                    }

                    SECTION("d")
                    {
                        counter_d++;
                        SUCCEED();
                    }
                }
            }
        });

    nx::test_schedule_config config;
    config.section_filters = {"a", "b"};

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    // a and b should be entered twice (once for c, once for d)
    CHECK(counter_a == 2);
    CHECK(counter_b == 2);
    CHECK(counter_c == 1);
    CHECK(counter_d == 1);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("test sections - no filter runs all sections")
{
    int counter_a = 0;
    int counter_b = 0;

    nx::test_registry reg;
    reg.add_declaration( //
        "testNoFilter", {},
        [&]
        {
            SECTION("a")
            {
                counter_a++;
                SUCCEED();
            }

            SECTION("b")
            {
                counter_b++;
                SUCCEED();
            }
        });

    nx::test_schedule_config config;
    // No section filters

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    CHECK(counter_a == 1);
    CHECK(counter_b == 1);

    CHECK(exec.count_total_tests() == 1);
    CHECK(exec.count_total_checks() == 2);
    CHECK(exec.count_failed_tests() == 0);
}
