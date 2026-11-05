#include <nexus/test.hh>
#include <shaped-graphics-language/driver/format.hh>
#include <shaped-graphics-language/driver/prelude.hh>

namespace
{
/// `source` formatted, which must succeed and leave no line as written.
[[nodiscard]] cc::string formatted(cc::string_view source)
{
    auto const r = sgl::format_source(source);
    if (r.has_error())
    {
        FAIL("the source has syntax errors").dump("error", r.error()).dump("source", source);
        return {};
    }
    CHECK(r.value().unformatted_lines.empty());
    return r.value().text;
}
} // namespace

TEST("sgl format - a line's tokens take SGL's spacing: after commas, around arrows and operators, none in brackets")
{
    CHECK(formatted("fun f(a:float,b :float)->float=>a+b*2.0\n") == "fun f(a:float, b :float) -> float => a + b * 2.0\n");
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
                                        "    let x = 1.0    // aligned\n"
                                        "    let y = 2.0  // aligned\n"
                                        "\n"
                                        "    return x+y\n"
                                        "\n\n\n");
    CHECK(formatted(source)
          == "// a  comment,with odd spacing\n"
             "fun f() -> float:\n"
             "    let x = 1.0    // aligned\n"
             "    let y = 2.0  // aligned\n"
             "\n"
             "    return x + y\n");
    // a test's message is a string, whose spacing is its content
    CHECK(formatted("test:\n    check(1 == 1, \"a,b  c\")\n") == "test:\n    check(1 == 1, \"a,b  c\")\n");
    // the file's line ends are kept, and a last line without one gets the file's
    CHECK(formatted("fun f() -> float:\r\n    return 1.0+2.0") == "fun f() -> float:\r\n    return 1.0 + 2.0\r\n");
}

TEST("sgl format - formatting twice changes nothing, and a source with syntax errors is refused, naming the first")
{
    auto const once = formatted("fun f(a:float,b:float)->float=>a+b\nfun g( ) -> float => f(1.0,2.0)\n");
    CHECK(formatted(once) == once);
    auto const broken = sgl::format_source("fun f() -> float:\n    return (1.0\n");
    REQUIRE(broken.has_error());
    CHECK(broken.error().starts_with("line 2: "));
}

TEST("sgl format - the prelude is formatted already")
{
    for (auto const& f : sgl::prelude_files())
        CHECK(formatted(f.source) == f.source).dump("file", f.name);
}
