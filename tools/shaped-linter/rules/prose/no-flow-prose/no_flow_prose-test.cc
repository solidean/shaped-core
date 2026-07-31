#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `no-flow-prose` — the scratchpad the rule was built in, and where an interesting
// regression gets pinned.
// Breadth lives in no_flow_prose.md, next to this file; see
// docs/coding-guidelines.md for which of the two a new case belongs in.

using namespace scl;

namespace
{
isize count_in(cc::string_view source, cc::string_view path)
{
    auto const found = run_rules_on_text(source, path);
    for (auto const& f : found)
        CHECK(f.rule_id == "no-flow-prose");
    return found.size();
}
} // namespace

TEST("shaped-linter - no-flow-prose - the heuristic itself")
{
    SECTION("a sentence ending mid-line fires")
    {
        CHECK(count_in("// One point. And a second one.\n", "a.cc") == 1);
    }
    SECTION("a sentence ending the line does not")
    {
        CHECK(count_in("// One point.\n// And a second one.\n", "a.cc") == 0);
    }
    SECTION("a long wrapped sentence carries no interior stop")
    {
        CHECK(count_in("// a sentence long enough that it had to wrap\n// onto a second line entirely.\n", "a.cc") == 0);
    }
    SECTION("only the first seam on a line is reported")
    {
        CHECK(count_in("// One. Two. Three.\n", "a.cc") == 1);
    }
    SECTION("trailing whitespace still counts as ending the line")
    {
        CHECK(count_in("// One point.   \n", "a.cc") == 0);
    }
}

TEST("shaped-linter - no-flow-prose - the exclusions")
{
    SECTION("e.g. and i.e. are not sentence ends")
    {
        CHECK(count_in("// a list of things, e.g. this one and that one\n", "a.cc") == 0);
        CHECK(count_in("// the other way round, i.e. backwards\n", "a.cc") == 0);
    }
    SECTION("a known abbreviation is not either")
    {
        CHECK(count_in("// vectors, strings, etc. are all in clean-core\n", "a.cc") == 0);
    }
    SECTION("a member access is not a sentence end")
    {
        CHECK(count_in("// call ctx.report to emit a finding\n", "a.cc") == 0);
    }
    SECTION("a decimal number is not")
    {
        CHECK(count_in("// requires CMake 3.28 or newer\n", "a.cc") == 0);
    }
    SECTION("an ordered-list marker is not")
    {
        CHECK(count_in("1. the first step, written out\n", "a.md") == 0);
        CHECK(count_in("12. the twelfth step, written out\n", "a.md") == 0);
    }
    SECTION("inline code is not read as prose")
    {
        CHECK(count_in("a call like `f(x). g(y)` in the middle of a line\n", "a.md") == 0);
    }
    SECTION("but a stop after inline code is")
    {
        CHECK(count_in("this is `cc::vector`, done. And here is the next point.\n", "a.md") == 1);
    }
}

TEST("shaped-linter - no-flow-prose - every language, and only its prose")
{
    SECTION("a Python comment")
    {
        CHECK(count_in("# One point. And a second one.\n", "a.py") == 1);
    }
    SECTION("a Python docstring")
    {
        CHECK(count_in("def f():\n    \"\"\"One point. And a second one.\"\"\"\n", "a.py") == 1);
    }
    SECTION("markdown body text")
    {
        CHECK(count_in("One point. And a second one.\n", "a.md") == 1);
    }
    SECTION("a fenced code block in markdown is never prose")
    {
        CHECK(count_in("```py\nx = 1  # One point. And a second.\n```\n", "a.md") == 0);
    }
    SECTION("C++ code is not prose, however many dots it has")
    {
        CHECK(count_in("auto s = \"One point. And a second one.\";\n", "a.cc") == 0);
    }
}

TEST("shaped-linter - no-flow-prose - what a finding carries")
{
    auto const found = run_rules_on_text("// One point. And a second one.\n", "a.cc");
    REQUIRE(found.size() == 1);

    // The carets underline the seam — the `.` and the space after it — so they sit where the break goes.
    CHECK(found[0].span.byte_end - found[0].span.byte_begin == 2);

    // There is deliberately no fix: splitting the line mechanically is not what obeying the rule means.
    CHECK(!found[0].suggested_fix.has_value());
    REQUIRE(found[0].suggested_hint.has_value());
    CHECK(found[0].suggested_hint.value().edits.size() == 0);
    CHECK(found[0].suggested_hint.value().message.contains("coding-guidelines.md"));
}
