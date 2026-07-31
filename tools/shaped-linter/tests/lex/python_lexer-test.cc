#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/python_lexer.hh>
#include <shaped-linter/lex/source_buffer.hh>

using namespace scl;

namespace
{
/// Owns the buffer AND its tokens: tokens borrow the buffer's text, so the buffer must outlive them.
/// Heap-boxed for a stable address, as in lexer-test.cc.
struct lexed
{
    cc::unique_ptr<source_buffer> buf;
    token_stream ts;

    /// The significant (non-trivia) tokens, dropping the trailing end_of_file.
    /// Indent/dedent are kept —
    /// they are the structure this lexer exists for.
    cc::vector<token> sig() const
    {
        cc::vector<token> out;
        for (auto const& t : ts.tokens)
            if (!t.is_trivia() && t.kind != token_kind::end_of_file)
                out.push_back(t);
        return out;
    }
};

lexed lex_text(cc::string_view s)
{
    auto buf = cc::make_unique<source_buffer>(source_buffer::from_text(cc::string(s), "x.py", 0));
    auto ts = lex_python(*buf).value();
    return {.buf = cc::move(buf), .ts = cc::move(ts)};
}

/// The indent (`>`) / dedent (`<`) markers of `s`, in order.
cc::string indent_shape(cc::string_view s)
{
    cc::string out;
    for (auto const& t : lex_text(s).ts.tokens)
    {
        if (t.is(token_kind::indent))
            out += '>';
        else if (t.is(token_kind::dedent))
            out += '<';
    }
    return out;
}
} // namespace

TEST("shaped-linter - python lexer - the basic kinds")
{
    auto const lx = lex_text("x = f(1, 'a')  # note\n");
    auto const t = lx.sig();
    REQUIRE(t.size() == 8);
    CHECK(t[0].kind == token_kind::identifier);
    CHECK(t[1].is_punct("="));
    CHECK(t[3].is_punct("("));
    CHECK(t[4].kind == token_kind::integer_literal);
    CHECK(t[6].kind == token_kind::string_literal);
    CHECK(t[6].text == "'a'");
}

TEST("shaped-linter - python lexer - literals")
{
    SECTION("a triple-quoted string spans lines")
    {
        auto const lx = lex_text("s = \"\"\"one\ntwo\"\"\"\n");
        auto const t = lx.sig();
        REQUIRE(t.size() == 3);
        CHECK(t[2].kind == token_kind::string_literal);
        CHECK(t[2].text == "\"\"\"one\ntwo\"\"\"");
    }
    SECTION("a prefix belongs to the string")
    {
        auto const lx = lex_text("p = rb'x\\'y'\n");
        auto const t = lx.sig();
        REQUIRE(t.size() == 3);
        CHECK(t[2].kind == token_kind::string_literal);
        CHECK(t[2].text == "rb'x\\'y'");
    }
    SECTION("an f-string is one token, braces and all")
    {
        auto const lx = lex_text("s = f\"a{b}c\"\n");
        CHECK(lx.sig()[2].text == "f\"a{b}c\"");
    }
    SECTION("numbers keep their shape")
    {
        auto const t = lex_text("a = 0x1f\nb = 1_000\nc = 1.5e-3\n").sig();
        CHECK(t[2].kind == token_kind::integer_literal);
        CHECK(t[5].kind == token_kind::integer_literal);
        CHECK(t[8].kind == token_kind::floating_literal);
    }
    SECTION("a keyword is a keyword and everything else an identifier")
    {
        auto const t = lex_text("def f(): return None\n").sig();
        CHECK(t[0].is_keyword("def"));
        CHECK(t[1].kind == token_kind::identifier);
        CHECK(t[5].is_keyword("return"));
        CHECK(t[6].is_keyword("None"));
    }
    SECTION("an unterminated string is recovered with a diagnostic")
    {
        auto const lx = lex_text("s = 'oops\n");
        CHECK(lx.ts.diagnostics.size() == 1);
        CHECK(lx.sig()[2].kind == token_kind::string_literal);
    }
}

TEST("shaped-linter - python lexer - the tokens tile the file")
{
    // Gap-free spans are what every downstream span-based tool assumes; the C++ stream promises the same.
    auto const lx = lex_text("def f():\n    return 1  # x\n");
    u32 next = 0;
    for (auto const& t : lx.ts.tokens)
    {
        CHECK(t.span.byte_begin == next);
        next = t.span.byte_end;
    }
    CHECK(next == u32(lx.buf->text().size()));
}

TEST("shaped-linter - python lexer - indentation")
{
    SECTION("a nested block indents and dedents once")
    {
        CHECK(indent_shape("def f():\n    return 1\nx = 2\n") == "><");
    }
    SECTION("two levels close in one step at the end")
    {
        CHECK(indent_shape("if a:\n    if b:\n        c()\n") == ">><<");
    }
    SECTION("a blank line does not change the level")
    {
        CHECK(indent_shape("def f():\n    a()\n\n    b()\nx = 1\n") == "><");
    }
    SECTION("a comment-only line does not either, whatever column it sits at")
    {
        CHECK(indent_shape("def f():\n    a()\n# a comment at column 0\n    b()\nx = 1\n") == "><");
    }
    SECTION("end of file closes every open level")
    {
        CHECK(indent_shape("if a:\n    if b:\n        c()") == ">><<");
    }
    SECTION("a tab counts to the next multiple of eight")
    {
        CHECK(indent_shape("if a:\n\tb()\nc()\n") == "><");
    }
}

TEST("shaped-linter - python lexer - a logical line survives what breaks a physical one")
{
    SECTION("a newline inside brackets does not open a line")
    {
        // Without bracket tracking the `2` would look like an indented block of its own.
        CHECK(indent_shape("f(\n    1,\n    2,\n)\n") == "");
    }
    SECTION("a backslash continuation is trivia")
    {
        CHECK(indent_shape("x = 1 + \\\n    2\ny = 3\n") == "");
    }
}
