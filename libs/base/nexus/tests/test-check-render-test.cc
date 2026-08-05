#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <stdexcept>

// How a failing check RENDERS; the counts are covered by test-check-test.cc.
// What is under test here is the text a developer actually sees: `test_error::expanded`, which is what both exporters serialize.
//
// The invariant: the auto-captured operands and the framework's own explanation are one field each, and every chained .context() / .note() / .dump() is appended after them.
// Nothing a user chains may be dropped, and nothing a user chains may displace what the framework had to say.

namespace
{
// Run `body` as a nested test and return the first failure it recorded.
// The body is expected to fail exactly once, and the inner execution absorbs it, so this test stays green.
nx::test_error const& first_error(nx::test_schedule_execution const& exec)
{
    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].root.errors.size() == 1);
    return exec.executions[0].root.errors[0];
}

template <class F>
nx::test_schedule_execution run_failing(F&& body)
{
    nx::test_registry reg;
    reg.add_declaration("subject", {}, body);
    auto schedule = nx::test_schedule::create({}, reg);
    return nx::execute_tests(schedule, {});
}
} // namespace

TEST("check render - a comparison keeps its operands and appends every annotation")
{
    auto const exec = run_failing(
        []
        {
            auto const v = 3;
            CHECK(1 == 2).context("during parse phase").note("checking the header").dump("v", v);
        });

    auto const& err = first_error(exec);

    // the decomposed comparison still leads
    CHECK(err.expanded.contains("1 == 2"));

    // ... and all three annotations survive behind it
    CHECK(err.expanded.contains("during parse phase"));
    CHECK(err.expanded.contains("note: checking the header"));
    CHECK(err.expanded.contains("v: 3"));

    // extra_lines is the structured copy: user annotations only, no operands mixed in
    CHECK(err.extra_lines.size() == 3);
    CHECK(err.extra_lines[0] == "during parse phase");
}

TEST("check render - a bare boolean appends its annotations too")
{
    auto const exec = run_failing(
        []
        {
            auto const flag = false;
            CHECK(flag).context("flag must be set after init");
        });

    auto const& err = first_error(exec);
    CHECK(err.expanded.contains("failed"));
    CHECK(err.expanded.contains("flag must be set after init"));
}

TEST("check render - a user context does not shadow the throws diagnostic")
{
    auto const exec = run_failing([] { CHECK_THROWS([] {}()).context("parsing an empty document"); });

    auto const& err = first_error(exec);
    CHECK(err.expanded.contains("no exception was thrown"));
    CHECK(err.expanded.contains("parsing an empty document"));
}

TEST("check render - a user context does not shadow the throws_as diagnostic")
{
    auto const exec = run_failing(
        []
        {
            CHECK_THROWS_AS([] { throw std::runtime_error("boom"); }(), std::logic_error) //
                .context("expected a logic error here");
        });

    auto const& err = first_error(exec);
    CHECK(err.expanded.contains("wrong exception type"));
    CHECK(err.expanded.contains("expected a logic error here"));
}

#if CC_ASSERT_ENABLED
TEST("check render - a user context does not shadow the asserts diagnostic")
{
    auto const exec = run_failing([] { CHECK_ASSERTS([] {}()).context("the guard should have fired"); });

    auto const& err = first_error(exec);
    CHECK(err.expanded.contains("no assertion was triggered"));
    CHECK(err.expanded.contains("the guard should have fired"));
}
#endif

TEST("check render - a passing check records nothing")
{
    nx::test_registry reg;
    reg.add_declaration("subject", {}, [] { CHECK(1 == 1).context("never shown").note("nor this"); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(exec.count_failed_checks() == 0);
    CHECK(exec.executions[0].root.errors.size() == 0);
}

TEST("check render - annotations reach the junit report")
{
    // The results XML is the only channel from a failing check to `dev.py test` / test_diag, so an
    // annotation that renders but does not export is still invisible where it matters.
    auto const exec = run_failing([] { CHECK(1 == 2).context("corpus.md:14"); });

    cc::string const xml = nx::write_junit_xml("render-suite", exec);
    CHECK(xml.contains("corpus.md:14"));
}
