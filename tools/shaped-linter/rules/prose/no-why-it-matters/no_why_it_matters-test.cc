#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `no-why-it-matters`; breadth lives in no_why_it_matters.md, next to this file.

using namespace scl;

namespace
{
isize count_in(cc::string_view source, cc::string_view path = "a.md")
{
    auto n = isize(0);
    for (auto const& f : run_rules_on_text(source, path))
        if (f.rule_id == "no-why-it-matters")
            ++n;
    return n;
}
} // namespace

TEST("shaped-linter - no-why-it-matters - the announcements fire")
{
    CHECK(count_in("## Why this matters\n") == 1);
    CHECK(count_in("**Why it matters:** a caller may pass any integer.\n") == 1);
    CHECK(count_in("This matters in mixed pipelines.\n") == 1);
    CHECK(count_in("It is stateless, which matters here.\n") == 1);
    CHECK(count_in("Tracks are ordered — which matters, because a viewer sorts nothing.\n") == 1);
    CHECK(count_in("Pseudo-interfaces are excluded, which matters more than it sounds.\n") == 1);
    CHECK(count_in("Taking the file's word matters more than it looks.\n") == 1);
    CHECK(count_in("Here's why: the browser cannot block.\n") == 1);
    CHECK(count_in("It names the module and — this is the part that matters — the offset.\n") == 1);
}

TEST("shaped-linter - no-why-it-matters - a real subject stays quiet")
{
    CHECK(count_in("The order matters.\n") == 0);
    CHECK(count_in("The count that matters is offered against taken.\n") == 0);
    CHECK(count_in("Measure where it matters.\n") == 0);
    CHECK(count_in("Decide which matters more.\n") == 0);
    CHECK(count_in("It says \"I will not read this, and here is why\".\n") == 0);
}

TEST("shaped-linter - no-why-it-matters - a block reads as one text")
{
    SECTION("a relative clause wrapped onto the next line is not a sentence start")
    {
        CHECK(count_in("// the only thing\n// that matters is the count\n", "a.cc") == 0);
    }
    SECTION("an announcement wrapped across lines is still found")
    {
        CHECK(count_in("// it is stateless,\n// which matters here\n", "a.cc") == 1);
    }
    SECTION("inline code is never a word of a phrase")
    {
        CHECK(count_in("a `this matters` string\n") == 0);
    }
    SECTION("code is not prose")
    {
        CHECK(count_in("auto s = \"This matters.\";\n", "a.cc") == 0);
    }
}

TEST("shaped-linter - no-why-it-matters - what a finding carries")
{
    auto const source = cc::string_view("// Stateless, which matters here.\n");
    auto const found = run_rules_on_text(source, "a.cc");
    REQUIRE(found.size() == 1);
    CHECK(source.subview({.start = found[0].span.byte_begin, .end = found[0].span.byte_end}) == ", which matters");
    CHECK(!found[0].suggested_fix.has_value());
    CHECK(found[0].suggested_hint.has_value());
}
