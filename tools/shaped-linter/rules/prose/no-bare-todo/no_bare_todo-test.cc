#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `no-bare-todo`; breadth lives in no_bare_todo.md, next to this file.

using namespace scl;

namespace
{
cc::vector<finding> found_in(cc::string_view source, cc::string_view path = "a.cc")
{
    auto out = cc::vector<finding>();
    for (auto const& f : run_rules_on_text(source, path))
        if (f.rule_id == "no-bare-todo")
            out.push_back(f);
    return out;
}
} // namespace

TEST("shaped-linter - no-bare-todo - the carets land on the word alone")
{
    auto const source = cc::string_view("// TODO: drop this\n");
    auto const found = found_in(source);
    REQUIRE(found.size() == 1);
    CHECK(found[0].span.byte_begin == 3);
    CHECK(found[0].span.byte_end == 7);
    CHECK(found[0].message.contains("TODO"));
    CHECK(!found[0].suggested_fix.has_value());
    CHECK(found[0].suggested_hint.has_value());
}

TEST("shaped-linter - no-bare-todo - one finding per marker, each where it is")
{
    CHECK(found_in("// TODO and FIXME\n").size() == 2);
    CHECK(found_in("// TODOTODO\n").size() == 0);
    CHECK(found_in("// TODO:\n// FIXME\n").size() == 2);
}

TEST("shaped-linter - no-bare-todo - a backtick pair closes on its own line")
{
    // An unbalanced backtick must not leave the next line's marker hidden.
    CHECK(found_in("// a stray ` here\n// TODO\n").size() == 1);
}

TEST("shaped-linter - no-bare-todo - markdown is not checked")
{
    CHECK(found_in("TODO: this is prose about one\n", "a.md").size() == 0);
}
