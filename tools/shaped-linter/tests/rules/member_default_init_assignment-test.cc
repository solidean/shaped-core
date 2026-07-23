#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

using namespace scl;

namespace
{
/// Assert the one finding on `source` suggests replacing with `want_fix` (the " = …" text).
void expect_single(cc::string_view source, cc::string_view want_fix)
{
    auto const found = run_rules_on_text(source);
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "member-default-init-assignment");
    REQUIRE(found[0].suggested_fix.has_value());
    auto const& edits = found[0].suggested_fix.value().edits;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].replacement == want_fix);
}

void expect_none(cc::string_view source)
{
    CHECK(run_rules_on_text(source).size() == 0);
}
} // namespace

TEST("shaped-linter - member-default-init - positive fixes")
{
    SECTION("single plain value drops braces")
    {
        expect_single("struct S { cc::atomic<cc::u32> x{0}; };", " = 0");
    }
    SECTION("nullptr drops braces")
    {
        expect_single("struct S { cc::atomic<ring*> _ring{nullptr}; };", " = nullptr");
    }
    SECTION("false drops braces")
    {
        expect_single("struct S { cc::atomic<bool> _scan_pending{false}; };", " = false");
    }
    SECTION("empty braces become empty-brace assignment")
    {
        expect_single("struct S { int value{}; };", " = {}");
    }
    SECTION("multi-element keeps braces")
    {
        expect_single("struct S { P p{a, b}; };", " = {a, b}");
    }
    SECTION("designated init keeps braces")
    {
        expect_single("struct S { P p{.a = 1}; };", " = {.a = 1}");
    }
    SECTION("single call with inner comma drops braces")
    {
        // The comma is inside f(...), not top-level, so this is a single value -> drop.
        expect_single("struct S { int n{f(a, b)}; };", " = f(a, b)");
    }
}

TEST("shaped-linter - member-default-init - negatives never fire")
{
    SECTION("already assignment form")
    {
        expect_none("struct S { int x = 0; };");
    }
    SECTION("local variable in a function body")
    {
        expect_none("void f() { int y{0}; }");
    }
    SECTION("static local in a member function")
    {
        expect_none("struct S { void f() { static cc::atomic<int> s{1}; } };");
    }
    SECTION("constructor init-list")
    {
        expect_none("struct S { S() : _x{0} {} int _x; };");
    }
    SECTION("namespace-scope variable")
    {
        expect_none("namespace n { cc::atomic<int> g{0}; }");
    }
    SECTION("aggregate init at a call site")
    {
        expect_none("void f() { g({1, 2}); }");
    }
}

// --- data-driven corpus via the invocable mechanism -------------------------------------------------

namespace
{
/// A unique key type (not a bare primitive — invocable matching is by decayed type).
struct lint_case
{
    cc::string name;
    cc::string source;
    cc::vector<cc::string> want_fixes; // one " = …" per expected finding, in order; empty = no findings
};
} // namespace

INVOCABLE_TEST("shaped-linter - member-default-init corpus", (lint_case const& c))
{
    auto const found = run_rules_on_text(c.source);
    REQUIRE(found.size() == c.want_fixes.size());
    for (isize i = 0; i < found.size(); ++i)
    {
        CHECK(found[i].rule_id == "member-default-init-assignment");
        REQUIRE(found[i].suggested_fix.has_value());
        auto const& edits = found[i].suggested_fix.value().edits;
        REQUIRE(edits.size() == 1);
        CHECK(edits[0].replacement == c.want_fixes[i]);
    }
}

TEST("shaped-linter - member-default-init corpus")
{
    cc::vector<lint_case> cases;
    cases.push_back(
        {.name = "atomic-u32", .source = "struct S { cc::atomic<cc::u32> strong{0}; };", .want_fixes = {" = 0"}});
    cases.push_back({.name = "empty-braces", .source = "struct S { int value{}; };", .want_fixes = {" = {}"}});
    cases.push_back({.name = "multi-element", .source = "struct S { P p{a, b}; };", .want_fixes = {" = {a, b}"}});
    cases.push_back({.name = "two-members",
                     .source = "struct S { cc::atomic<int> a{0}; cc::atomic<int> b{1}; };",
                     .want_fixes = {" = 0", " = 1"}});
    cases.push_back({.name = "neg-assignment", .source = "struct S { int y = 0; };", .want_fixes = {}});
    cases.push_back({.name = "neg-local", .source = "void f() { int y{0}; }", .want_fixes = {}});

    for (auto const& c : cases)
        nx::invoke_tests(c.name, c);
}
