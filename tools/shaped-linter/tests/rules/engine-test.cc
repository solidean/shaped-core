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
    CHECK(lint_and_fix("struct S { cc::atomic<cc::u32> x{0}; };") == "struct S { cc::atomic<cc::u32> x = 0; };");
}

TEST("shaped-linter - fix round-trip - nullptr and false")
{
    CHECK(lint_and_fix("struct S { cc::atomic<ring*> _ring{nullptr}; };")
          == "struct S { cc::atomic<ring*> _ring = nullptr; };");
    CHECK(lint_and_fix("struct S { cc::atomic<bool> _p{false}; };") == "struct S { cc::atomic<bool> _p = false; };");
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
             "  cc::atomic<cc::i64> _top = 0;\n"
             "  cc::atomic<ring*> _ring = nullptr;\n"
             "};");
}

TEST("shaped-linter - fix round-trip - normalizes spacing before the brace")
{
    // A space before the brace is absorbed: `x {0}` -> `x = 0`.
    CHECK(lint_and_fix("struct S { int x {0}; };") == "struct S { int x = 0; };");
}
