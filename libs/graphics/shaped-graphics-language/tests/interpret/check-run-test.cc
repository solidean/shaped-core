#include "../check/check-test-support.hh"

#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/interpret/interpret.hh>
#include <shaped-graphics-language/legalize/core.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
/// Test `index` of `source` run, with the program checked clean first.
outcome run_test(checked_sources const& checked, isize index, run_limits const& limits = {})
{
    REQUIRE(reports_of(checked) == "");
    REQUIRE(index < checked.module.tests.size());
    auto const unit = checked.module.tests[index].unit;
    REQUIRE(unit >= 0);
    return interpret(checked.module, checked.module.test_units[unit], {}, limits);
}

/// The nodes of the site a failure names.
tree_view<flat_check_node> nodes_of(checked_sources const& checked, isize test, check_failure const& f)
{
    auto const& unit = checked.module.test_units[checked.module.tests[test].unit];
    return unit.at(unit.check_sites[f.site].nodes);
}
} // namespace

TEST("sgl run - a test whose checks hold runs clean, and counts them")
{
    auto const checked = check_sources(read_prelude(), "fun square(x: float) -> float => x * x\n"
                                                       "test:\n"
                                                       "    square 3.0 == 9.0\n"
                                                       "    let x = 10\n"
                                                       "    x * x > 50\n");
    auto const o = run_test(checked, 0);
    CHECK(cc::format("{} {}", to_string(o.status), o.detail) == "ok ");
    CHECK(o.checks_run == 2);
    CHECK(o.failures.empty());
}

TEST("sgl run - a false check is recorded and the test goes on, and a false assert stops it")
{
    // EVAL-75
    auto const going_on = check_sources(read_prelude(), "test:\n    1 > 2\n    2 > 3\n    true\n");
    auto const o = run_test(going_on, 0);
    CHECK(o.status == run_status::ok);
    CHECK(o.checks_run == 3);
    CHECK(o.failures.size() == 2);

    // EVAL-76: an assert of a function the test calls
    auto const stopping = check_sources(read_prelude(), "fun half(x: float) -> float:\n"
                                                        "    assert x >= 0.0\n"
                                                        "    return x * 0.5\n"
                                                        "test:\n"
                                                        "    half(-1.0) < 0.0\n"
                                                        "    true\n");
    auto const s = run_test(stopping, 0);
    CHECK(s.status == run_status::assertion_failed);
    // an assert is a check that ran, and the test's own check never did
    CHECK(s.checks_run == 1);
    REQUIRE(s.failures.size() == 1);
    CHECK(s.failures[0].values[1].leaves[0].as_float() == -1.0f);
}

TEST("sgl run - a failure keeps the value of every node, and a skipped operand as not evaluated")
{
    // CHK-229: `x > 5` is false, so the right side of the `and` never ran
    auto const checked = check_sources(read_prelude(), "test:\n    let x = 1\n    x > 5 and x - 1 == 1\n");
    auto const o = run_test(checked, 0);
    REQUIRE(o.failures.size() == 1);
    auto const& f = o.failures[0];
    auto const nodes = nodes_of(checked, 0, f);
    REQUIRE(nodes.size() == 7);
    CHECK(nodes[0].kind == check_node_kind::and_);
    CHECK(nodes[1].kind == check_node_kind::compare);
    CHECK(nodes[1].op == ">");
    CHECK(f.is_evaluated[1]);
    CHECK(!f.values[1].leaves[0].as_bool());
    CHECK(f.values[nodes[1].lhs].leaves[0].as_int() == 1);
    CHECK(f.values[nodes[1].rhs].leaves[0].as_int() == 5);
    // the right side of the `and`, and both of its operands
    CHECK(!f.is_evaluated[4]);
    CHECK(!f.is_evaluated[5]);
    CHECK(!f.is_evaluated[6]);
}

TEST("sgl run - a chain fails at its first false link, and a later link never runs")
{
    auto const checked = check_sources(read_prelude(), "test 1 < 3 < 2 < 10\n");
    auto const o = run_test(checked, 0);
    REQUIRE(o.failures.size() == 1);
    auto const& f = o.failures[0];
    auto const nodes = nodes_of(checked, 0, f);
    CHECK(nodes[0].kind == check_node_kind::chain);
    auto first_false = isize(-1);
    auto links = 0;
    for (auto i = isize(0); i < nodes.size(); ++i)
    {
        if (nodes[i].kind != check_node_kind::compare)
            continue;
        ++links;
        if (first_false < 0 && f.is_evaluated[i] && !f.values[i].leaves[0].as_bool())
            first_false = i;
    }
    CHECK(links == 3);
    REQUIRE(first_false >= 0);
    CHECK(f.values[nodes[first_false].lhs].leaves[0].as_int() == 3);
    CHECK(f.values[nodes[first_false].rhs].leaves[0].as_int() == 2);
    // the link `2 < 10` behind it did not run, nor did its new operand
    CHECK(!f.is_evaluated[nodes.size() - 1]);
}

TEST("sgl run - a failure in a loop of the test keeps the loop's variable, and the count is capped")
{
    auto const checked = check_sources(read_prelude(), "test:\n    for i in 0 ..< 20:\n        i < 2\n");
    auto const o = run_test(checked, 0, {.max_failures = 3});
    CHECK(o.status == run_status::ok);
    CHECK(o.checks_run == 20);
    REQUIRE(o.failures.size() == 3);
    CHECK(o.failures_dropped == 15);
    REQUIRE(o.failures[0].loop_values.size() == 1);
    CHECK(o.failures[0].loop_values[0].leaves[0].as_int() == 2);
    CHECK(o.failures[2].loop_values[0].leaves[0].as_int() == 4);
}

TEST("sgl run - an assert is no part of what a target writes, and the two forms agree without it")
{
    auto const checked
        = check_sources(read_prelude(), "struct frag:\n    a: float\n@pixel struct target:\n    color: float4\n"
                                        "@pixel fun main_ps(p: frag) -> target:\n"
                                        "    assert p.a >= 0.0 and p.a < 2.0\n"
                                        "    return target(float4(p.a, p.a, p.a, 1.0))\n");
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& structured = checked.module.entry_points[0];
    auto const core = legalize(checked.module, structured);
    CHECK(!find_core_violation(core).has_value());
    CHECK(core.check_sites.empty());

    auto inputs = run_inputs{.parameter = zero_value(checked.module, structured.input)};
    inputs.parameter.leaves[0] = scalar::of(-1.0f);
    CHECK(interpret(checked.module, structured, inputs).status == run_status::assertion_failed);
    CHECK(dump(interpret(checked.module, structured, inputs, {.run_checks = false}))
          == dump(interpret(checked.module, core, inputs)));

    for (auto const t : sgl::emit::all_targets())
        CHECK(sgl::emit::dump_errors(sgl::emit::emit(checked.module, 0, t)) == "");
}
