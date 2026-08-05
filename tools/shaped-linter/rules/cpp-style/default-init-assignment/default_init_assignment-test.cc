#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `default-init-assignment` — the scratchpad the rule was built in, and where an interesting regression gets pinned.
// Breadth lives in default_init_assignment.md, next to this file.
// See docs/coding-guidelines.md for which of the two a new case belongs in.

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

/// The one finding's hint, requiring that it has one.
hint const& only_hint(cc::span<finding const> found)
{
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].suggested_hint.has_value());
    return found[0].suggested_hint.value();
}
} // namespace

TEST("shaped-linter - default-init - positive fixes")
{
    SECTION("a single value keeps its braces")
    {
        expect_single("struct S { cc::atomic<u32> x{0}; };", " = {0}");
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

// The hint channel, which `--fix` never applies (engine-test.cc pins that).
// What matters here is that the right shapes get one, that its edit is the braceless form, and that its wording never invents a type.
TEST("shaped-linter - default-init - the member hint drops the braces")
{
    SECTION("a whitelisted type is stated without a caveat")
    {
        auto const found = run_rules_on_text("struct S { cc::atomic<bool> _p{false}; };");
        auto const& h = only_hint(found);
        REQUIRE(h.edits.size() == 1);
        CHECK(h.edits[0].replacement == " = false");
        CHECK(h.message.contains("confirmed"));
        CHECK(h.message.contains("cc::atomic<bool>"));
    }
    SECTION("an unverified type carries the escape hatch, naming the type")
    {
        auto const found = run_rules_on_text("struct S { cc::mutex<int*> _actor{nullptr}; };");
        auto const& h = only_hint(found);
        REQUIRE(h.edits.size() == 1);
        CHECK(h.edits[0].replacement == " = nullptr");
        CHECK(h.message.contains("= cc::mutex<int*>(nullptr)"));
    }
    SECTION("a specifier ahead of the type suppresses the escape hatch rather than misquoting it")
    {
        // `alignas(64) cc::atomic<int>` is not a type spelling, so it must never be offered as one.
        auto const found = run_rules_on_text("struct S { alignas(64) cc::atomic<int> _top{0}; };");
        auto const& h = only_hint(found);
        REQUIRE(h.edits.size() == 1);
        CHECK(h.edits[0].replacement == " = 0");
        CHECK(!h.message.contains("alignas"));
    }
    SECTION("a later declarator's hint does not read the earlier declarator as a type")
    {
        auto const found = run_rules_on_text("struct S { int a{1}, b{2}; };");
        REQUIRE(found.size() == 2);
        REQUIRE(found[1].suggested_hint.has_value());
        auto const& h = found[1].suggested_hint.value();
        REQUIRE(h.edits.size() == 1);
        CHECK(h.edits[0].replacement == " = 2");
        CHECK(!h.message.contains("int a"));
    }
}

TEST("shaped-linter - default-init - no braceless form means no hint")
{
    SECTION("empty braces")
    {
        auto const found = run_rules_on_text("struct S { int value{}; };");
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_hint.has_value());
    }
    SECTION("a list")
    {
        auto const found = run_rules_on_text("struct S { P p{a, b}; };");
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_hint.has_value());
    }
    SECTION("a designated initializer")
    {
        auto const found = run_rules_on_text("struct S { P p{.a = 1}; };");
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_hint.has_value());
    }
    SECTION("a comma inside a nested group is still one value")
    {
        auto const found = run_rules_on_text("struct S { int n{f(a, b)}; };");
        auto const& h = only_hint(found);
        REQUIRE(h.edits.size() == 1);
        CHECK(h.edits[0].replacement == " = f(a, b)");
    }
}

TEST("shaped-linter - default-init - the non-member hint is the auto form, prose only")
{
    SECTION("a plain declaration names the type")
    {
        auto const found = run_rules_on_text("void f() { cc::random rng{u64(seed)}; }");
        auto const& h = only_hint(found);
        CHECK(h.edits.size() == 0); // `{…}` -> `(…)` is not a rewrite we may hand to --fix
        CHECK(h.message.contains("auto rng = cc::random(u64(seed));"));
    }
    SECTION("a specifier leaves the type as a placeholder")
    {
        auto const found = run_rules_on_text("void f() { static cc::atomic<int> s{1}; }");
        auto const& h = only_hint(found);
        CHECK(h.edits.size() == 0);
        CHECK(h.message.contains("auto s = <type>(1);"));
    }
    SECTION("east const likewise — the const would have to move")
    {
        auto const found = run_rules_on_text("void f() { cc::vector<int> const v{1}; }");
        auto const& h = only_hint(found);
        CHECK(h.message.contains("auto v = <type>(1);"));
    }
    SECTION("value-init has no auto form worth suggesting")
    {
        auto const found = run_rules_on_text("void f() { T v{}; }");
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_hint.has_value());
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
