#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/prose/verify.hh>

// What `prose verify` lets through and what it catches: a reflow passes, a moved word or token does not.

using namespace scl;

namespace
{
bool passes(cc::string_view path, cc::string_view before, cc::string_view after)
{
    auto const v = verify_prose_edit(path, before, after);
    return !v.unreadable && v.code_change.empty() && v.word_change.empty();
}
} // namespace

TEST("shaped-linter - prose verify - a reflow passes")
{
    SECTION("C++ comment lines rebroken")
    {
        CHECK(passes("a.cc", "// One point. And a\n// second one.\nint x = 1;\n", "// One point.\n// And a second one.\nint x = 1;\n"));
    }
    SECTION("a word hyphenated across a line break, rejoined")
    {
        CHECK(passes("a.cc", "// a cross-\n// check of both\n", "// a cross-check of both\n"));
    }
    SECTION("a markdown blockquote and list rebroken")
    {
        CHECK(passes("a.md", "> One point. And a\n> second one.\n\n- item one, and\n  more of it\n",
                     "> One point.\n> And a second one.\n\n- item one, and more of it\n"));
    }
    SECTION("Rust and Lean comments")
    {
        CHECK(passes("a.rs", "/// One. Two.\nfn f() {}\n", "/// One.\n/// Two.\nfn f() {}\n"));
        CHECK(passes("a.lean", "/-- One. Two. -/\ndef x := 1\n", "/-- One.\nTwo. -/\ndef x := 1\n"));
    }
}

TEST("shaped-linter - prose verify - what it catches")
{
    SECTION("a dropped full stop is a word change")
    {
        auto const v = verify_prose_edit("a.hh", "/// A `u8`. The rest.\n", "/// A `u8`\n/// The rest.\n");
        CHECK(v.code_change.empty());
        CHECK(!v.word_change.empty());
    }
    SECTION("a changed token is a code change")
    {
        auto const v = verify_prose_edit("a.cc", "// note\nint x = 1;\n", "// note\nint x = 2;\n");
        CHECK(!v.code_change.empty());
        CHECK(v.word_change.empty());
    }
    SECTION("a string literal is code, even one holding a comment marker")
    {
        CHECK(!verify_prose_edit("a.rs", "let s = \"// a\";\n", "let s = \"// b\";\n").code_change.empty());
    }
    SECTION("a fenced block in markdown is code")
    {
        CHECK(!verify_prose_edit("a.md", "text\n\n```\nx = 1\n```\n", "text\n\n```\nx = 2\n```\n").code_change.empty());
    }
    SECTION("a Python docstring is prose, not code")
    {
        auto const v = verify_prose_edit("a.py", "def f():\n    \"\"\"Old words.\"\"\"\n", "def f():\n    \"\"\"New words.\"\"\"\n");
        CHECK(v.code_change.empty());
        CHECK(!v.word_change.empty());
    }
}
