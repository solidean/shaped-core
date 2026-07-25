#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `default-init-assignment` — the scratchpad the rule was built in, and where an
// interesting regression gets pinned. Breadth lives in tests/rules/corpus/default_init_assignment.md;
// see docs/coding-guidelines.md for which of the two a new case belongs in.

using namespace scl;

namespace
{
/// Assert the one finding on `source` suggests replacing with `want_fix` (the " = …" text).
void expect_single(cc::string_view source, cc::string_view want_fix)
{
    auto const found = run_rules_on_text(source);
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "default-init-assignment");
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

TEST("shaped-linter - default-init - positive fixes")
{
    SECTION("a single value keeps its braces")
    {
        expect_single("struct S { cc::atomic<cc::u32> x{0}; };", " = {0}");
    }
    SECTION("nullptr keeps its braces")
    {
        expect_single("struct S { cc::atomic<ring*> _ring{nullptr}; };", " = {nullptr}");
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
    SECTION("a call in the initializer is copied verbatim")
    {
        expect_single("struct S { int n{f(a, b)}; };", " = {f(a, b)}");
    }
}

TEST("shaped-linter - default-init - fires outside record bodies too")
{
    SECTION("function local")
    {
        expect_single("void f() { int y{0}; }", " = {0}");
    }
    SECTION("static local in a member function")
    {
        expect_single("struct S { void f() { static cc::atomic<int> s{1}; } };", " = {1}");
    }
    SECTION("namespace-scope variable")
    {
        expect_single("namespace n { cc::atomic<int> g{0}; }", " = {0}");
    }
    SECTION("local inside a nested block")
    {
        expect_single("void f() { if (c) { int y{2}; } }", " = {2}");
    }
    SECTION("local inside a lambda body")
    {
        expect_single("void f() { auto g = [] { int y{3}; }; }", " = {3}");
    }
}

TEST("shaped-linter - default-init - negatives never fire")
{
    SECTION("already assignment form")
    {
        expect_none("struct S { int x = 0; };");
    }
    SECTION("constructor init-list")
    {
        expect_none("struct S { S() : _x{0} {} int _x; };");
    }
    SECTION("constructor init-list with several members")
    {
        expect_none("struct S { S() : _a{0}, _b{1} {} int _a; int _b; };");
    }
    SECTION("aggregate init at a call site")
    {
        expect_none("void f() { g({1, 2}); }");
    }
    SECTION("braced return value")
    {
        expect_none("P f() { return P{1, 2}; }");
    }
    SECTION("braced temporary as a statement")
    {
        expect_none("void f() { T{1}; }");
    }
    SECTION("array init at namespace scope keeps its own shape")
    {
        expect_none("int a[] = {1, 2};");
    }
}
