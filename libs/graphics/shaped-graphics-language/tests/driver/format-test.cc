#include <nexus/test.hh>
#include <shaped-graphics-language/driver/format.hh>
#include <shaped-graphics-language/driver/prelude.hh>

namespace
{
/// `source` formatted, which must skip nothing and leave no line as written.
[[nodiscard]] cc::string formatted(cc::string_view source)
{
    auto const r = sgl::format_source(source);
    CHECK(r.skipped.empty()).dump("source", source);
    CHECK(r.unformatted_lines.empty());
    return r.text;
}
} // namespace

TEST("sgl format - a line's tokens take SGL's spacing: after commas, around arrows and operators, none in brackets")
{
    CHECK(formatted("fun f(a:float,b :float)->float=>a+b*2.0\n") == "fun f(a: float, b: float) -> float => a + b * 2.0\n");
    CHECK(formatted("fun f( v: vec3 ) -> float  =>   dot( v , v )\n") == "fun f(v: vec3) -> float => dot(v, v)\n");
    CHECK(formatted("fun f() -> float:\n    let a = { x = 1.0 }\n    return a.x\n")
          == "fun f() -> float:\n    let a = {x = 1.0}\n    return a.x\n");
    CHECK(formatted("fun f() -> int:\n    let mut s=0\n    for i in 0..<4:\n        s+=i\n    return s\n")
          == "fun f() -> int:\n    let mut s = 0\n    for i in 0 ..< 4:\n        s += i\n    return s\n");
}

TEST("sgl format - what touching or standing apart means is kept: numbers, prefix operators, calls and block arrows")
{
    // `1e-3` is one number the tokenizer split, and `-x` a prefix
    CHECK(formatted("fun f(x: float) -> float => 1e-3*x  *  -x\n") == "fun f(x: float) -> float => 1e-3 * x * -x\n");
    // a value block's `=>:` keeps its colon
    CHECK(formatted("fun f(x: float) -> float:\n    let y = case x > 0.0:\n        true =>:\n            yield 1.0\n"
                    "        false => 0.0\n    return y\n")
          == "fun f(x: float) -> float:\n    let y = case x > 0.0:\n        true =>:\n            yield 1.0\n"
             "        false => 0.0\n    return y\n");
}

TEST("sgl format - comments, strings and indentation stay as written, and trailing whitespace and end blanks go")
{
    auto const source = cc::string_view("// a  comment,with odd spacing   \n"
                                        "fun f() -> float:   \n"
                                        "    let x = 1.0\n"
                                        "\n"
                                        "    return x+y\n"
                                        "\n\n\n");
    CHECK(formatted(source)
          == "// a  comment,with odd spacing\n"
             "fun f() -> float:\n"
             "    let x = 1.0\n"
             "\n"
             "    return x + y\n");
    // a test's message is a string, whose spacing is its content
    CHECK(formatted("test:\n    check(1 == 1, \"a,b  c\")\n") == "test:\n    check(1 == 1, \"a,b  c\")\n");
    // the file's line ends are kept, and a last line without one gets the file's
    CHECK(formatted("fun f() -> float:\r\n    return 1.0+2.0") == "fun f() -> float:\r\n    return 1.0 + 2.0\r\n");
}

TEST("sgl format - formatting twice changes nothing")
{
    auto const once
        = formatted("fun f(a:float,b:float)->float=>a+b  // sum\nfun g( ) -> float => f(1.0,2.0)  // three\n");
    CHECK(formatted(once) == once);
}

TEST("sgl format - a colon touches what stands before it and is followed by one space, `let x: T` included")
{
    CHECK(formatted("struct light:\n    power :float\n") == "struct light:\n    power: float\n");
    CHECK(formatted("fun f(v :vec3) -> float:\n    let mut d :float = dot(v, v)\n    return d\n")
          == "fun f(v: vec3) -> float:\n    let mut d: float = dot(v, v)\n    return d\n");
}

TEST("sgl format - the trailing comments of consecutive lines line up past the longest code, within a limit")
{
    // one space past the longest; a blank line, or a line without one, ends the run
    CHECK(formatted("struct light:\n    power: float // watts\n    tint: float3       // linear\n\n    x: int  // "
                    "own\n")
          == "struct light:\n    power: float // watts\n    tint: float3 // linear\n\n    x: int // own\n");
    // a line whose comment would move further than the limit starts a run of its own
    CHECK(formatted("fun f() -> float:\n    let a = 1.0 // a\n    let much_longer_name_than_the_limit = 2.0 // b\n"
                    "    return a\n")
          == "fun f() -> float:\n    let a = 1.0 // a\n    let much_longer_name_than_the_limit = 2.0 // b\n"
             "    return a\n");
}

TEST("sgl format - a syntax error leaves its block and the block's siblings as written, and formats the rest")
{
    auto const r = sgl::format_source("fun broken(a:float) -> float:\n"
                                      "    let x=(1.0\n"
                                      "    return a+x\n"
                                      "fun fine(a:float) -> float => a*2.0\n");
    CHECK(r.text
          == "fun broken(a: float) -> float:\n"
             "    let x=(1.0\n"
             "    return a+x\n"
             "fun fine(a: float) -> float => a * 2.0\n");
    REQUIRE(r.skipped.size() == 1);
    CHECK(r.skipped[0].first_line == 1);
    CHECK(r.skipped[0].last_line == 2);
    CHECK(r.skipped[0].error_line == 1);
    CHECK(r.unformatted_lines.empty());
    // at the top level, the declaration that holds the error is what stays
    auto const top = sgl::format_source("fun f(a:float) -> float => (a\nfun g(a:float) -> float => a*2.0\n");
    CHECK(top.text == "fun f(a:float) -> float => (a\nfun g(a: float) -> float => a * 2.0\n");
}

TEST("sgl format - the prelude is formatted already")
{
    for (auto const& f : sgl::prelude_files())
        CHECK(formatted(f.source) == f.source).dump("file", f.name);
}

TEST("sgl format - code is indented four spaces a level, a tab included, and what a comment or string owns moves along")
{
    CHECK(formatted("fun f(a: float) -> float:\n  if a > 0.0:\n     return a\n  return 0.0\n")
          == "fun f(a: float) -> float:\n    if a > 0.0:\n        return a\n    return 0.0\n");
    // a tab in the indentation is an error with one reading, which the formatter fixes
    CHECK(formatted("fun f() -> float:\n\treturn 0.0\n") == "fun f() -> float:\n    return 0.0\n");
    // a comment's own lines keep their place below it, and a string's content its value (STR-16)
    CHECK(formatted("fun f() -> float:\n  // a note\n    continued\n  let s = \"\n      hi\n        deeper\n  \"\n"
                    "  return 0.0\n")
          == "fun f() -> float:\n    // a note\n      continued\n    let s = \"\n        hi\n          deeper\n    \"\n"
             "    return 0.0\n");
}
