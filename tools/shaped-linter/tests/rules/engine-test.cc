#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

using namespace scl;

namespace
{
/// Lint `src`, then apply the findings' suggested edits back to `src` — the whole detect-and-fix path.
cc::string lint_and_fix(cc::string_view src)
{
    auto const found = run_rules_on_text(src);
    cc::vector<text_edit> edits;
    for (auto const& f : found)
        if (f.suggested_fix.has_value())
            for (auto const& e : f.suggested_fix.value().edits)
                edits.push_back({.span = e.span, .replacement = e.replacement});
    return apply_edits(src, edits);
}
} // namespace

TEST("shaped-linter - apply_edits - basic splice")
{
    auto const edits = cc::vector<text_edit>{
        {.span = {.file_id = 0, .byte_begin = 1, .byte_end = 2}, .replacement = "XY"},
    };
    CHECK(apply_edits("abc", edits) == "aXYc");
}

TEST("shaped-linter - apply_edits - multiple edits back-to-front")
{
    // Replace [0,1) with "AA" and [2,3) with "CC" in "abc" -> "AAbCC".
    auto const edits = cc::vector<text_edit>{
        {.span = {.file_id = 0, .byte_begin = 0, .byte_end = 1}, .replacement = "AA"},
        {.span = {.file_id = 0, .byte_begin = 2, .byte_end = 3}, .replacement = "CC"},
    };
    CHECK(apply_edits("abc", edits) == "AAbCC");
}

TEST("shaped-linter - fix round-trip - single value")
{
    // The braces stay: `= 0` is copy-init, which an aggregate or an explicit constructor refuses.
    CHECK(lint_and_fix("struct S { cc::atomic<cc::u32> x{0}; };") == "struct S { cc::atomic<cc::u32> x = {0}; };");
}

TEST("shaped-linter - fix round-trip - nullptr and false")
{
    CHECK(lint_and_fix("struct S { cc::atomic<ring*> _ring{nullptr}; };")
          == "struct S { cc::atomic<ring*> _ring = {nullptr}; };");
    CHECK(lint_and_fix("struct S { cc::atomic<bool> _p{false}; };") == "struct S { cc::atomic<bool> _p = {false}; };");
}

TEST("shaped-linter - fix round-trip - empty braces")
{
    CHECK(lint_and_fix("struct S { int value{}; };") == "struct S { int value = {}; };");
}

TEST("shaped-linter - fix round-trip - keep multi-element and designated")
{
    CHECK(lint_and_fix("struct S { P p{a, b}; };") == "struct S { P p = {a, b}; };");
    CHECK(lint_and_fix("struct S { P p{.a = 1}; };") == "struct S { P p = {.a = 1}; };");
}

TEST("shaped-linter - fix round-trip - several members at once")
{
    CHECK(lint_and_fix("struct S {\n"
                       "  cc::atomic<cc::i64> _top{0};\n"
                       "  cc::atomic<ring*> _ring{nullptr};\n"
                       "};")
          == "struct S {\n"
             "  cc::atomic<cc::i64> _top = {0};\n"
             "  cc::atomic<ring*> _ring = {nullptr};\n"
             "};");
}

TEST("shaped-linter - fix round-trip - a function local")
{
    CHECK(lint_and_fix("void f() { int y{0}; }") == "void f() { int y = {0}; }");
}

TEST("shaped-linter - fix round-trip - a local inside a lambda body")
{
    CHECK(lint_and_fix("void f() { auto g = [] { int y{1}; }; }") == "void f() { auto g = [] { int y = {1}; }; }");
}

TEST("shaped-linter - fix round-trip - a namespace-scope variable")
{
    CHECK(lint_and_fix("namespace n { cc::atomic<int> g{0}; }") == "namespace n { cc::atomic<int> g = {0}; }");
}

TEST("shaped-linter - fix round-trip - normalizes spacing before the brace")
{
    // A space before the brace is absorbed: `x {0}` -> `x = {0}`.
    CHECK(lint_and_fix("struct S { int x {0}; };") == "struct S { int x = {0}; };");
}

TEST("shaped-linter - fix round-trip - an array bound survives the rewrite")
{
    // The whole point of the applied round-trip: a corpus `fix=` only pins the replacement TEXT, so it
    // cannot see that the edit started too far left and ate the `[N]`.
    CHECK(lint_and_fix("struct S { T a[N]{1, 2}; };") == "struct S { T a[N] = {1, 2}; };");
    CHECK(lint_and_fix("struct S { int a[3]{0}; };") == "struct S { int a[3] = {0}; };");
    CHECK(lint_and_fix("void f() { int a[2][3]{1}; }") == "void f() { int a[2][3] = {1}; }");
}

TEST("shaped-linter - apply_fixes ignores the hint channel")
{
    // The one behavior that makes a hint a hint. This member carries both channels — fix `= {false}` and
    // hint `= false` — and the applied result must be the fix, with the hint's edit left on the floor.
    auto const src = cc::string_view("struct S { cc::atomic<bool> _p{false}; };");
    auto const found = run_rules_on_text(src);
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].suggested_hint.has_value());
    REQUIRE(found[0].suggested_hint.value().edits.size() == 1);

    CHECK(lint_and_fix(src) == "struct S { cc::atomic<bool> _p = {false}; };");
}

TEST("shaped-linter - fix round-trip - every declarator of a multi-declarator statement")
{
    CHECK(lint_and_fix("struct S { int a{1}, b{2}; };") == "struct S { int a = {1}, b = {2}; };");
    CHECK(lint_and_fix("void f() { int a{1}, b{2}, c{3}; }") == "void f() { int a = {1}, b = {2}, c = {3}; }");
    CHECK(lint_and_fix("struct S { int a{1}, b[2]{3}; };") == "struct S { int a = {1}, b[2] = {3}; };");

    // the declarators that are not brace-initialized are left exactly as they were
    CHECK(lint_and_fix("struct S { int a{1}, b = 2, c; };") == "struct S { int a = {1}, b = 2, c; };");
}
