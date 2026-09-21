#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

TEST("sgl groups - comments vanish, parens become one group, fusing is kept")
{
    auto const file = sgl::parse("let m = f(1, (2)) [3] // note\n");

    CHECK(sgl::dump_groups(file) == "let m = f~(1~, (2)) [3]\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - a block colon makes the children statements, and only as the last token")
{
    auto const file = sgl::parse("if x: // why\n"
                                 "    let y : int = 1\n"
                                 "    while y:\n"
                                 "        y\n"
                                 "z\n");

    CHECK(sgl::dump_groups(file)
          == "if x:\n"
             "  let y : int = 1\n"
             "  while y:\n"
             "    y\n"
             "z\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - an empty block is an error that an indented comment silences")
{
    CHECK(sgl::dump_diagnostics(sgl::parse("if x:\ny\n")) == "empty-block @4+1\n");
    CHECK(sgl::dump_diagnostics(sgl::parse("if x:\n    // nothing to do\ny\n")) == "");
}

TEST("sgl groups - children of a line that ends plainly continue it, and a leading dot is fused by rule")
{
    auto const file = sgl::parse("print 10\n"
                                 "    + 20\n"
                                 "    + 30\n"
                                 "let v = builder\n"
                                 "    .scale(2)\n"
                                 "    .build()\n");

    CHECK(sgl::dump_groups(file)
          == "print 10 + 20 + 30\n"
             "let v = builder~.~scale~(2)~.~build~()\n");
}

TEST("sgl groups - a continuation line may carry the block colon, and the body is then its child")
{
    auto const file = sgl::parse("if a\n"
                                 "    and b:\n"
                                 "        c\n");

    CHECK(sgl::dump_groups(file)
          == "if a and b:\n"
             "  c\n");
}

TEST("sgl groups - inside an open paren every child line is a run of elements, closed at the start of the sibling")
{
    auto const file = sgl::parse("let m = mat3(\n"
                                 "    1, 0\n"
                                 "    0, 1,\n"
                                 ")\n"
                                 "let r = [foo(\n"
                                 "    1\n"
                                 "    2\n"
                                 ") + bar(\n"
                                 "    3\n"
                                 ")]\n");

    CHECK(sgl::dump_groups(file)
          == "let m = mat3~(| 1~, 0 | 0~, 1~,)\n"
             "let r = [foo~(| 1 | 2) + bar~(| 3)]\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - an element line that opens with an infix operator continues the element above it")
{
    auto const file = sgl::parse("if (\n"
                                 "    a\n"
                                 "    + b > 100\n"
                                 "    and c\n"
                                 "    -d\n"
                                 "):\n"
                                 "    draw\n");

    // `+ b` and `and c` cannot start an element, so they go on; `-d` is a prefix operator and starts one.
    CHECK(sgl::dump_groups(file)
          == "if (| a + b > 100 and c | -~d):\n"
             "  draw\n");
    CHECK(sgl::dump_forms(file)
          == "(kw kw:if (round (run (run (run id:a op:+ id:b) op:> num:100) op:and id:c) (prefix - id:d))\n"
             "  id:draw)\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - a paren the sibling does not close is closed where its children ended")
{
    auto const file = sgl::parse("foo(\n"
                                 "    1\n"
                                 "    )\n"
                                 "bar ]\n"
                                 "baz\n");

    // The closer in a child line matches nothing there, and the sibling is read fresh.
    CHECK(sgl::dump_groups(file)
          == "foo~(| 1<unclosed>\n"
             "bar\n"
             "baz\n");
    CHECK(sgl::dump_diagnostics(file)
          == "unmatched-closer @15+1\n"
             "missing-closer @3+1\n"
             "unmatched-closer @21+1\n");
}

TEST("sgl groups - only the string has to close at once; a paren may stay open across the closing line")
{
    auto const file = sgl::parse("log(\"\n"
                                 "    hello\n"
                                 "\" + \"\n"
                                 "    world\n"
                                 "      indented\n"
                                 "\")\n"
                                 "next\n");

    CHECK(sgl::dump_groups(file)
          == "log~(\"hello\" + \"world\\nindented\")\n"
             "next\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - a block colon wins over an open paren, which the sibling still closes")
{
    auto const file = sgl::parse("on_click(handler = e =>:\n"
                                 "    print e\n"
                                 ")\n"
                                 "next\n");

    CHECK(sgl::dump_groups(file)
          == "on_click~(handler = e =>:\n"
             "  print e)\n"
             "next\n");
    CHECK(sgl::dump_diagnostics(file) == "");
}

TEST("sgl groups - attributes leave the run and hang off a neighbour")
{
    auto const file = sgl::parse("@builtin\n"
                                 "@range(0, 1) // both reach the next line that is not only attributes\n"
                                 "\n"
                                 "const bias = 0.5 @slider\n"
                                 "fun f(@a x: int, y: float @b(2)) -> @c vec4\n");

    CHECK(sgl::dump_groups(file)
          == "const{@builtin}{@range(0~, 1)} bias = 0~.~5{@slider}\n"
             "fun f~(x{@a}~: int~, y~: float{@b(2)}) -> vec4{@c}\n");
    CHECK(sgl::dump_diagnostics(file) == "");

    CHECK(sgl::dump_diagnostics(sgl::parse("x\n@dangling\n")) == "unattached-attribute @2+9\n");
}

TEST("sgl groups - where an attribute may stand")
{
    // Leading its line, trailing it, anywhere in a paren element, and directly after a marker are all fine.
    CHECK(sgl::dump_diagnostics(sgl::parse("@a fun f(x: int @b, @c y: int) -> @d int @e\n")) == "");

    CHECK(sgl::dump_diagnostics(sgl::parse("fun @vertex main()\n")) == "misplaced-attribute @4+7\n");
    CHECK(sgl::dump_diagnostics(sgl::parse("@range (0, 1)\nconst bias = 0.5\n")) == "spaced-attribute-arguments @0+6\n");

    // An attribute never leaves the block it was written in, so the statement after the block stays clean.
    auto const file = sgl::parse("fun f():\n    return\n    @inline\nlet x = 1\n");
    CHECK(sgl::dump_diagnostics(file) == "unattached-attribute @24+7\n");
    CHECK(sgl::dump_groups(file) == "fun f~():\n  return\nlet x = 1\n");
}

TEST("sgl groups - a continuation that continues again is reported and still read as one line")
{
    auto const file = sgl::parse("print 10\n"
                                 "    * 20\n"
                                 "        + 10\n"
                                 "    * 30\n");

    CHECK(sgl::dump_groups(file) == "print 10 * 20 + 10 * 30\n");
    CHECK(sgl::dump_diagnostics(file) == "nested-continuation @26+1\n");
}
