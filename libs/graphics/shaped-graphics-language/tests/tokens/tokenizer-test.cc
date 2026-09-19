#include <clean-core/common/assert.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>
#include <shaped-graphics-language/tokens/tokenizer.hh>

using namespace cc::primitive_defines;

namespace
{
/// The tokens of a one-line source, without the `code:` lead.
cc::string tokens_of(cc::string_view source)
{
    auto const file = sgl::parse(source);
    auto dump = sgl::dump_tokens(file);
    CC_ASSERT(dump.starts_with("code: ") && dump.ends_with("\n"), "expected exactly one code line");
    return dump.substring({.start = 6, .end = dump.size() - 1});
}

/// Tokens must tile their line: in order, inside it, and with nothing but spaces and tabs between them.
/// That is what makes whitespace recoverable without storing it.
bool tokens_tile_their_lines(sgl::parsed_file const& file)
{
    for (auto const& l : file.lines)
    {
        auto at = l.text.offset;
        for (auto const& t : file.tokens_of(l))
        {
            if (t.where.offset < at || t.where.end() > l.text.end() || t.where.empty())
                return false;
            for (auto c : file.text_of({.offset = at, .length = t.where.offset - at}))
                if (c != ' ' && c != '\t')
                    return false;
            at = t.where.end();
        }
        for (auto c : file.text_of({.offset = at, .length = l.text.end() - at}))
            if (c != ' ' && c != '\t')
                return false;
    }
    return true;
}
} // namespace

TEST("sgl tokenizer - a symbol is one run, whatever it will turn out to be")
{
    CHECK(tokens_of("let x") == "symbol(let) symbol(x)");
    CHECK(tokens_of("10a7 @range #ff00bb \\phi_1 dθ")
          == "symbol(10a7) symbol(@range) symbol(#ff00bb) symbol(\\phi_1) symbol(dθ)");
    CHECK(tokens_of("x' 1'000'000") == "symbol(x') symbol(1'000'000)");
    CHECK(tokens_of("_ _a a_") == "wildcard(_) symbol(_a) symbol(a_)");
}

TEST("sgl tokenizer - a dash is never part of a symbol")
{
    // Names get mirrored into host languages that cannot spell a dash, so `a-b` stays a subtraction.
    // Whether it touches its neighbours is kept, which is all a later phase needs to object to the spelling.
    CHECK(tokens_of("n_dot_l") == "symbol(n_dot_l)");
    CHECK(tokens_of("a-b") == "symbol(a)~op(-)~symbol(b)");
    CHECK(tokens_of("a - b") == "symbol(a) op(-) symbol(b)");
    CHECK(tokens_of("a -b") == "symbol(a) op(-)~symbol(b)");
    CHECK(tokens_of("a->b") == "symbol(a)~arrow(->)~symbol(b)");
    CHECK(tokens_of("x-=1") == "symbol(x)~op(-=)~symbol(1)");
    CHECK(tokens_of("-1") == "op(-)~symbol(1)");
    CHECK(tokens_of("a--b") == "symbol(a)~op(--)~symbol(b)");
    CHECK(tokens_of("a- b") == "symbol(a)~op(-) symbol(b)");

    // A signed exponent is three fused tokens, the same way `1.5` is; assembling the number is the form phase's job.
    CHECK(tokens_of("1e-5") == "symbol(1e)~op(-)~symbol(5)");
    CHECK(tokens_of("1e+5") == "symbol(1e)~op(+)~symbol(5)");
}

TEST("sgl tokenizer - operators are max-munched, and two dots open one")
{
    CHECK(tokens_of("a <= b != c") == "symbol(a) op(<=) symbol(b) op(!=) symbol(c)");
    CHECK(tokens_of("x=-1") == "symbol(x)~op(=-)~symbol(1)");
    CHECK(tokens_of("(a) -> b => c")
          == "round_open(()~symbol(a)~round_close()) arrow(->) symbol(b) double_arrow(=>) symbol(c)");
    CHECK(tokens_of("=>= ->>") == "op(=>=) op(->>)");

    // `0.` must not be read as a number, or a range could not be written.
    CHECK(tokens_of("0..<4") == "symbol(0)~op(..<)~symbol(4)");
    CHECK(tokens_of("a ..= b .. c") == "symbol(a) op(..=) symbol(b) op(..) symbol(c)");
    CHECK(tokens_of("1.5 v.x .point") == "symbol(1)~dot(.)~symbol(5) symbol(v)~dot(.)~symbol(x) dot(.)~symbol(point)");
}

TEST("sgl tokenizer - punctuation, and fusing as a property of the gap")
{
    CHECK(tokens_of("f(x)") == "symbol(f)~round_open(()~symbol(x)~round_close())");
    CHECK(tokens_of("f (x)") == "symbol(f) round_open(()~symbol(x)~round_close())");
    CHECK(tokens_of("a[i]{}") == "symbol(a)~square_open([)~symbol(i)~square_close(])~curly_open({)~curly_close(})");
    CHECK(tokens_of("x: t, y : t;") == "symbol(x)~colon(:) symbol(t)~comma(,) symbol(y) colon(:) symbol(t)~semicolon(;)");
    CHECK(tokens_of("a::b") == "symbol(a)~double_colon(::)~symbol(b)");
}

TEST("sgl tokenizer - a comment runs to the end of its line and wins over an operator run")
{
    CHECK(tokens_of("x // note") == "symbol(x) comment(// note)");
    CHECK(tokens_of("a *// b") == "symbol(a) op(*)~comment(// b)");
    CHECK(tokens_of("a / / b") == "symbol(a) op(/) op(/) symbol(b)");
}

TEST("sgl tokenizer - a comment-only line owns the lines below it, a trailing comment does not")
{
    auto const file = sgl::parse("// head\n"
                                 "   detail\n"
                                 "      deeper let x = \"\n"
                                 "    back\n"
                                 "if x: // trailing\n"
                                 "    y\n");

    CHECK(sgl::dump_tokens(file)
          == "code: comment(// head)\n"
             "  comment: comment(detail)\n"
             "    comment: comment(deeper let x = \")\n"
             "    comment: comment(back)\n" // four columns is still deeper than `detail`'s three
             "code: symbol(if) symbol(x)~colon(:) comment(// trailing)\n"
             "  code: symbol(y)\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl tokenizer - quoted literals on one line")
{
    CHECK(tokens_of("print \"hello world\"") == "symbol(print) quote_open(\")~string_body(hello world)~quote_close(\")");
    CHECK(tokens_of("\"\"") == "quote_open(\")~quote_close(\")");
    CHECK(tokens_of("\"a \\\" b\"") == "quote_open(\")~string_body(a \\\" b)~quote_close(\")");
    CHECK(tokens_of("'x' `y`")
          == "quote_open(')~string_body(x)~quote_close(') quote_open(`)~string_body(y)~quote_close(`)");

    // Inside a string nothing is code, a comment marker included.
    CHECK(tokens_of("\"a // b\" c") == "quote_open(\")~string_body(a // b)~quote_close(\") symbol(c)");

    // After a symbol character a single quote continues the symbol instead of opening a literal.
    CHECK(tokens_of("f' 'c'") == "symbol(f') quote_open(')~string_body(c)~quote_close(')");
}

TEST("sgl tokenizer - an undelimited string ends with its line and costs nothing after it")
{
    auto const file = sgl::parse("print \"hello // not a comment\n"
                                 "let x = 1\n");

    CHECK(sgl::dump_tokens(file)
          == "code: symbol(print) quote_open(\")~string_body(hello // not a comment)\n"
             "code: symbol(let) symbol(x) op(=) symbol(1)\n");
    CHECK(sgl::dump_diagnostics(file) == "undelimited-string @6+1\n");
}

TEST("sgl tokenizer - a line ending in an open quote makes its children the string and its sibling the closer")
{
    auto const file = sgl::parse("print \"\n"
                                 "    hello \"world\" // plain\n"
                                 "        deeper\n"
                                 "\n"
                                 "  short\n"
                                 "\n"
                                 "\" + tail\n"
                                 "next\n");

    CHECK(sgl::dump_tokens(file)
          == "code: symbol(print) quote_open(\")\n"
             "  string_content: string_body(hello \"world\" // plain)\n"
             "    string_content: string_body(deeper)\n"
             "  blank\n"
             "  string_content: string_body(short)\n"
             "blank\n"
             "code: quote_close(\") op(+) symbol(tail)\n"
             "code: symbol(next)\n");
    // `short` has two columns where four are taken off, which costs it nothing but a diagnostic.
    CHECK(sgl::dump_diagnostics(file) == "underindented-string-content @53+1\n");
}

TEST("sgl tokenizer - trailing whitespace after an open quote still opens a multi-line string")
{
    auto const file = sgl::parse("print \"  \n    body\n\"\n");

    CHECK(sgl::dump_tokens(file)
          == "code: symbol(print) quote_open(\")\n"
             "  string_content: string_body(body)\n"
             "code: quote_close(\")\n");
}

TEST("sgl tokenizer - a multi-line string that is never closed damages only its own sibling")
{
    SECTION("the next sibling does not start with the closer")
    {
        auto const file = sgl::parse("print \"\n"
                                     "    body\n"
                                     "let x = 1\n");

        CHECK(sgl::dump_tokens(file)
              == "code: symbol(print) quote_open(\")\n"
                 "  string_content: string_body(body)\n"
                 "code: symbol(let) symbol(x) op(=) symbol(1)\n");
        CHECK(sgl::dump_diagnostics(file) == "missing-string-end @17+1\n");
    }

    SECTION("there is no next sibling, so the parent's next line is untouched")
    {
        auto const file = sgl::parse("fun f():\n"
                                     "    print \"\n"
                                     "        body\n"
                                     "\"x\"\n");

        CHECK(sgl::dump_tokens(file)
              == "code: symbol(fun) symbol(f)~round_open(()~round_close())~colon(:)\n"
                 "  code: symbol(print) quote_open(\")\n"
                 "    string_content: string_body(body)\n"
                 "code: quote_open(\")~string_body(x)~quote_close(\")\n");
        CHECK(sgl::dump_diagnostics(file) == "missing-string-end @19+1\n");
    }
}

TEST("sgl tokenizer - unknown bytes become one error token and one diagnostic per run")
{
    auto const file = sgl::parse("a $$ b\n");

    CHECK(sgl::dump_tokens(file) == "code: symbol(a) error($$) symbol(b)\n");
    CHECK(sgl::dump_diagnostics(file) == "unknown-character @2+2\n");
}

TEST("sgl tokenizer - tokens tile every line of any input")
{
    for (auto const source : {
             cc::string_view(""),
             cc::string_view("   \n\t\n"),
             cc::string_view("fun shade(m: material) -> vec3:\n    let k = m.albedo * 0.5 // half\n    return k\n"),
             cc::string_view("print \"\n  a\n      b\n\"  +  1 \t\n"),
             cc::string_view("\"unterminated \\"),
             cc::string_view("a\r\n  $ \x01\x02 ) ] }\r\n//\r\n   x\r"),
             cc::string_view("'\n`\n\"\n"),
         })
    {
        auto const file = sgl::parse(source);
        CHECK(tokens_tile_their_lines(file));
        CHECK(sgl::print_source(file) == source);
    }
}

TEST("sgl tokenizer - a double-quoted string is split at its interpolations")
{
    CHECK(tokens_of("\"n = $count!\"")
          == "quote_open(\")~string_body(n = )~dollar($)~symbol(count)~string_body(!)~quote_close(\")");
    CHECK(tokens_of("\"$p.x, $p.y.\"")
          == "quote_open(\")~dollar($)~symbol(p)~dot(.)~symbol(x)~string_body(, )~dollar($)~symbol(p)~dot(.)~symbol(y)"
             "~string_body(.)~quote_close(\")");

    // A name inside a string is narrower than a symbol, so an apostrophe after it is text.
    CHECK(tokens_of("\"$name's\"") == "quote_open(\")~dollar($)~symbol(name)~string_body('s)~quote_close(\")");

    // `$$` is a dollar, and stays in the body for whoever computes the value.
    CHECK(tokens_of("\"costs $$5\"") == "quote_open(\")~string_body(costs $$5)~quote_close(\")");

    // The other quotes do not interpolate.
    CHECK(tokens_of("'$x'") == "quote_open(')~string_body($x)~quote_close(')");
}

TEST("sgl tokenizer - the parentheses of an interpolation hold code, strings included")
{
    CHECK(tokens_of("\"len $(length(p) * 2)m\"")
          == "quote_open(\")~string_body(len )~dollar($)~round_open(()~symbol(length)~round_open(()~symbol(p)"
             "~round_close()) op(*) symbol(2)~round_close())~string_body(m)~quote_close(\")");

    // A quote inside the parentheses opens a string of its own instead of closing the outer one.
    CHECK(tokens_of("\"a $(f(\")\")) b\"")
          == "quote_open(\")~string_body(a )~dollar($)~round_open(()~symbol(f)~round_open(()~quote_open(\")"
             "~string_body())~quote_close(\")~round_close())~round_close())~string_body( b)~quote_close(\")");

    // An interpolation ends with its line whatever happens, and so does the string around it.
    auto const file = sgl::parse("print \"oops $(a + \nnext\n");
    CHECK(sgl::dump_diagnostics(file) == "missing-closer @13+1\nundelimited-string @6+1\n");
    CHECK(sgl::dump_tokens(file).ends_with("code: symbol(next)\n"));
}

TEST("sgl tokenizer - what a string may not hold says so and keeps its meaning")
{
    CHECK(sgl::dump_diagnostics(sgl::parse("\"cost in $ is unknown\"")) == "stray-dollar @9+1\n");
    CHECK(sgl::dump_diagnostics(sgl::parse("\"a \\q b\"")) == "unknown-escape @3+2\n");
    CHECK(sgl::dump_diagnostics(sgl::parse("\"\\n \\t \\\\ \\\" \\0 \\u{1F600}\"")) == "");
    CHECK(sgl::dump_diagnostics(sgl::parse("\"\\u{} \\u{1234567} \\u12\""))
          == "unknown-escape @1+2\nunknown-escape @6+2\nunknown-escape @18+2\n");
}

TEST("sgl tokenizer - a multi-line string interpolates and escapes nothing")
{
    auto const file = sgl::parse("print \"\n"
                                 "    a \\n is two characters, and $count is $$1\n"
                                 "\"\n");

    CHECK(sgl::dump_tokens(file)
          == "code: symbol(print) quote_open(\")\n"
             "  string_content: string_body(a \\n is two characters, and )~dollar($)~symbol(count)~string_body( is "
             "$$1)\n"
             "code: quote_close(\")\n");
    CHECK(sgl::dump_diagnostics(file) == "");
    CHECK(sgl::dump_groups(file) == "print \"a \\n is two characters, and $count is $$1\"\n");
}

TEST("sgl tokenizer - reserved openers are openers, so nothing below them changes meaning later")
{
    SECTION("three quotes are closed by three quotes")
    {
        auto const file = sgl::parse("let raw = \"\"\"\n    text with $no interpolation\n\"\"\" + tail\n");

        CHECK(sgl::dump_tokens(file)
              == "code: symbol(let) symbol(raw) op(=) quote_open(\"\"\")\n"
                 "  string_content: string_body(text with $no interpolation)\n"
                 "code: quote_close(\"\"\") op(+) symbol(tail)\n");
        CHECK(sgl::dump_diagnostics(file) == "reserved-string-opener @10+3\n");
    }

    SECTION("a quote and one name is a tag only when lines hang below it")
    {
        auto const tagged = sgl::parse("let s = \"json\n    { \"a\": $a }\n\"\n");
        CHECK(sgl::dump_tokens(tagged)
              == "code: symbol(let) symbol(s) op(=) quote_open(\")~symbol(json)\n"
                 "  string_content: string_body({ \"a\": )~dollar($)~symbol(a)~string_body( })\n"
                 "code: quote_close(\")\n");
        CHECK(sgl::dump_diagnostics(tagged) == "reserved-string-opener @8+5\n");

        // Without lines below, it is what it looks like: a one-word string somebody has not closed yet.
        auto const typing = sgl::parse("print \"hello\nlet x = 1\n");
        CHECK(sgl::dump_diagnostics(typing) == "undelimited-string @6+1\n");
    }
}
