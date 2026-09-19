#include <clean-core/common/assert.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/debug/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace cc::primitive_defines;

namespace
{
/// The form of a one-statement source, which must parse without a diagnostic.
cc::string form_of(cc::string_view source)
{
    auto const file = sgl::parse(source);
    auto dump = sgl::dump_forms(file);
    CC_ASSERT(dump.ends_with("\n"), "expected at least one form");
    dump.resize_down_to(dump.size() - 1);
    auto const diagnostics = sgl::dump_diagnostics(file);
    if (!diagnostics.empty())
        dump += " !! " + diagnostics;
    return dump;
}

cc::string diagnostics_of(cc::string_view source)
{
    return sgl::dump_diagnostics(sgl::parse(source));
}
} // namespace

TEST("sgl forms - atoms")
{
    CHECK(form_of("x") == "id:x");
    CHECK(form_of("_") == "wildcard:_");
    CHECK(form_of("#ff00bb") == "hash:#ff00bb");
    CHECK(form_of("\"hi there\"") == "str:\"hi there\"");
    CHECK(form_of(".point") == "dot:point");
    CHECK(form_of(".0") == "dot:0");
    CHECK(form_of("(1, x, )") == "(round num:1 id:x)");
    CHECK(form_of("[]") == "(square)");
    CHECK(form_of("{a, b = 2}") == "(curly id:a (run id:b op:= num:2))");
}

TEST("sgl forms - a number is assembled from fused tokens")
{
    CHECK(form_of("1.5") == "num:1.5");
    CHECK(form_of("1'000'000") == "num:1'000'000");
    CHECK(form_of("1.5e-3f32") == "num:1.5e-3f32");
    CHECK(form_of("1e+5") == "num:1e+5");
    CHECK(form_of("0x1p-3") == "num:0x1p-3");
    CHECK(form_of("-3") == "num:-3");
    CHECK(form_of("-x") == "(prefix - id:x)");

    // In a hex literal `e` is a digit, so this is a subtraction — and one that needs spaces.
    CHECK(form_of("0x1e-3") == "(run num:0x1e op:- num:3) !! operator-needs-spaces @4+1\n");

    // A trailing dot belongs to the number unless a name is fused to it.
    CHECK(form_of("1.") == "num:1.");
    CHECK(form_of("1.max(2)") == "(call (member max num:1) (round num:2))");
    CHECK(form_of("0..<4") == "(run num:0 op:..< num:4)");
    CHECK(form_of("t.0.1") == "(member 1 (member 0 id:t))");
}

TEST("sgl forms - fused lists and members are postfix; an unfused list is an argument")
{
    CHECK(form_of("f(x).y[i]") == "(call (member y (call id:f (round id:x))) (square id:i))");
    CHECK(form_of("f (x)") == "(apply id:f (round id:x))");
    CHECK(form_of("buffer[float]") == "(call id:buffer (square id:float))");
}

TEST("sgl forms - application binds tighter than every binary operator")
{
    CHECK(form_of("cross a b") == "(apply id:cross id:a id:b)");
    CHECK(form_of("foo a + bar b") == "(run (apply id:foo id:a) op:+ (apply id:bar id:b))");
    CHECK(form_of("f -x y") == "(apply id:f (prefix - id:x) id:y)");
    CHECK(form_of("f - x") == "(run id:f op:- id:x)");
    CHECK(form_of("f .point") == "(apply id:f dot:point)");
}

TEST("sgl forms - the binary levels")
{
    CHECK(form_of("a + b * c - d") == "(run id:a op:+ (run id:b op:* id:c) op:- id:d)");
    CHECK(form_of("a & m == 0") == "(run (run id:a op:& id:m) op:== num:0)");
    CHECK(form_of("0 <= i < n") == "(run num:0 op:<= id:i op:< id:n)");
    CHECK(form_of("a and not b") == "(run id:a op:and (prefix not id:b))");
    CHECK(form_of("x as int in 0..=10 : bool") == "(run id:x op:as id:int op:in (run num:0 op:..= num:10) op:: id:bool)");
    CHECK(form_of("x as int < 5") == "(run (run id:x op:as id:int) op:< num:5)");
    CHECK(form_of("x as float * 2") == "(run id:x op:as (run id:float op:* num:2))");
    CHECK(form_of("(f : (int) -> int)") == "(round (run id:f op:: (round id:int) op:-> id:int))");
}

TEST("sgl forms - assignment and computes-as nest to the right, loosest of all")
{
    CHECK(form_of("a = b = c") == "(run id:a op:= (run id:b op:= id:c))");
    CHECK(form_of("x += y * 2") == "(run id:x op:+= (run id:y op:* num:2))");
    CHECK(form_of("x => y => x + y") == "(run id:x op:=> (run id:y op:=> (run id:x op:+ id:y)))");
    CHECK(form_of("f = x => x + 1") == "(run id:f op:= (run id:x op:=> (run id:x op:+ num:1)))");
    CHECK(form_of("a; b = 1") == "(seq id:a (run id:b op:= num:1))");
}

TEST("sgl forms - a keyword form takes whole comma-separated expressions")
{
    CHECK(form_of("return a + 2") == "(kw kw:return (run id:a op:+ num:2))");
    CHECK(form_of("print \"total:\", total") == "(kw kw:print str:\"total:\" id:total)");
    CHECK(form_of("assert a == b, \"sizes differ\"") == "(kw kw:assert (run id:a op:== id:b) str:\"sizes differ\")");
    CHECK(form_of("break") == "(kw kw:break)");
    CHECK(form_of("let x : int = 10") == "(run (kw kw:let (run id:x op:: id:int)) op:= num:10)");
    CHECK(form_of("let mut color = m.albedo * k")
          == "(run (kw kw:let kw:mut id:color) op:= (run (member albedo id:m) op:* id:k))");
    CHECK(form_of("let v = cross a b : vec3")
          == "(run (kw kw:let id:v) op:= (run (apply id:cross id:a id:b) op:: id:vec3))");
    CHECK(form_of("use materials as mat") == "(kw kw:use (run id:materials op:as id:mat))");
    CHECK(form_of("if done => return") == "(run (kw kw:if id:done) op:=> (kw kw:return))");
    CHECK(form_of("fun lerp(a: float, t: float = 0.5) -> float => a * t")
          == "(run (kw kw:fun (run (call id:lerp (round (run id:a op:: id:float) (run (run id:t op:: id:float) op:= "
             "num:0.5))) "
             "op:-> id:float)) op:=> (run id:a op:* id:t))");

    // Inside a paren a comma ends the element, so a keyword form there takes one expression.
    CHECK(form_of("f(return a, b)") == "(call id:f (round (kw kw:return id:a) id:b))");
}

TEST("sgl forms - a block belongs to the rightmost form of its line")
{
    CHECK(form_of("for i in 0..<n:\n    total += i\n")
          == "(kw kw:for (run id:i op:in (run num:0 op:..< id:n))\n"
             "  (run id:total op:+= id:i))");

    CHECK(form_of("let k = case kind:\n    .point => 1.0\n    _ => 0.0\n")
          == "(run (kw kw:let id:k) op:= (kw kw:case id:kind\n"
             "  (run dot:point op:=> num:1.0)\n"
             "  (run wildcard:_ op:=> num:0.0)))");

    CHECK(form_of("if a:\n    b\nelse if c:\n    d\n")
          == "(kw kw:if id:a\n"
             "  id:b)\n"
             "(kw kw:else kw:if id:c\n"
             "  id:d)");

    CHECK(form_of("on_click(handler = e =>:\n    print e\n)\n")
          == "(call id:on_click (round (run id:handler op:= (run id:e op:=>\n"
             "  (kw kw:print id:e)))))");

    CHECK(form_of("struct material:\n    albedo: vec3\n    roughness: float = 0.5\n")
          == "(kw kw:struct id:material\n"
             "  (run id:albedo op:: id:vec3)\n"
             "  (run (run id:roughness op:: id:float) op:= num:0.5))");
}

TEST("sgl forms - what is forbidden among equals still parses")
{
    CHECK(form_of("a and b or c") == "(run id:a op:and id:b op:or id:c) !! mixed-operators @0+12\n");
    CHECK(form_of("not a and b") == "(run (prefix not id:a) op:and id:b) !! misplaced-not @0+5\n");
    CHECK(form_of("a & b | c") == "(run id:a op:& id:b op:| id:c) !! mixed-operators @0+9\n");
    CHECK(form_of("a & b & c") == "(run id:a op:& id:b op:& id:c)");

    CHECK(diagnostics_of("a < b > c") == "non-monotone-comparison @0+9\n");
    CHECK(diagnostics_of("a != b != c") == "non-monotone-comparison @0+11\n");
    CHECK(diagnostics_of("a == b == c") == "");
    CHECK(diagnostics_of("a <= b == c < d") == "");
    CHECK(diagnostics_of("0..<1..<2") == "chained-range @0+9\n");
}

TEST("sgl forms - operator spacing is syntax")
{
    CHECK(form_of("a-b") == "(run id:a op:- id:b) !! operator-needs-spaces @1+1\n");
    CHECK(form_of("(-b)") == "(round (prefix - id:b))");
    CHECK(form_of("f(a, -b)") == "(call id:f (round id:a (prefix - id:b)))");
    CHECK(form_of("a * -b") == "(run id:a op:* (prefix - id:b))");
    CHECK(form_of("x! + 1") == "(run (postfix ! id:x) op:+ num:1) !! reserved-operator @1+1\n");
    CHECK(form_of("a <=> b") == "(run id:a op:<=> id:b) !! unknown-operator @2+3\n");

    // A range may touch its operands, and nothing else may: `->` and `=>` owe the same spaces as `+`.
    CHECK(form_of("0 ..< 4") == "(run num:0 op:..< num:4)");
    CHECK(form_of("a->b") == "(run id:a op:-> id:b) !! operator-needs-spaces @1+2\n");
    CHECK(form_of("x=>y") == "(run id:x op:=> id:y) !! operator-needs-spaces @1+2\n");
    CHECK(form_of("(x: int)") == "(round (run id:x op:: id:int))");
}

TEST("sgl forms - attributes belong to their element, except directly after a marker")
{
    CHECK(form_of("@builtin\nstruct bool\n") == "(kw kw:struct id:bool){@builtin}");
    CHECK(form_of("const bias = 0.5 @range(0, 1)") == "(run (kw kw:const id:bias) op:= num:0.5){@range(0~, 1)}");
    CHECK(form_of("fun f(@a x: int, y: float @b) -> @c vec4")
          == "(kw kw:fun (run (call id:f (round (run id:x op:: id:int){@a} (run id:y op:: id:float){@b})) op:-> "
             "id:vec4{@c}))");
}

TEST("sgl forms - broken source still yields a tree, and the damage stays where it is")
{
    CHECK(form_of("a +") == "(run id:a op:+ missing) !! expected-expression @2+1\n");
    CHECK(form_of("let = 5") == "(run (kw kw:let) op:= num:5)");
    CHECK(form_of("x ) y") == "(apply id:x id:y) !! unmatched-closer @2+1\n");
    CHECK(form_of("a , b") == "(error id:a error:, error:b) !! unexpected-token @2+1\nunexpected-token @4+1\n");

    auto const file = sgl::parse("fun f(:\n    let x = (1 +\n    let y = 2\nlet z = 3\n");
    CHECK(sgl::dump_forms(file).ends_with("(run (kw kw:let id:z) op:= num:3)\n"));
}

TEST("sgl forms - a number that does not follow the grammar is still a number form")
{
    CHECK(form_of("10a7") == "num:10a7 !! malformed-number @0+4\n");
    CHECK(form_of("1_000") == "num:1_000 !! underscore-in-number @0+5\n");
    CHECK(form_of("1''0") == "num:1''0 !! malformed-number @0+4\n");
    CHECK(form_of("0x") == "num:0x !! malformed-number @0+2\n");
    CHECK(form_of("1e") == "num:1e !! malformed-number @0+2\n");

    // Which widths exist is a later question; the shape of a suffix is this one's.
    CHECK(form_of("0b1010u8") == "num:0b1010u8");
    CHECK(form_of("0xff'ffu32") == "num:0xff'ffu32");
    CHECK(form_of("1f999") == "num:1f999");
    CHECK(form_of("2.5p3") == "num:2.5p3");
}

TEST("sgl forms - spellings that are kept free say what to write instead")
{
    CHECK(form_of("!ready") == "(prefix ! id:ready) !! reserved-operator @0+1\n");
    CHECK(form_of("a ! b") == "(run id:a op:! id:b) !! reserved-operator @2+1\n");
    CHECK(form_of("0 .. 4") == "(run num:0 op:.. num:4) !! bare-range @2+2\n");
    CHECK(form_of("light_kind::point") == "(member point id:light_kind) !! double-colon @10+2\n");
    CHECK(form_of("f(a; b)") == "(call id:f (round (seq id:a id:b))) !! semicolon-in-parens @3+1\n");
    CHECK(form_of("*x") == "(prefix * id:x) !! unknown-operator @0+1\n");

    // The splat is prefix, and the postfix spelling is kept free for a half-open range.
    CHECK(form_of("(..n, 0)") == "(round (prefix .. id:n) num:0)");
    CHECK(form_of("(n.., 0)") == "(round (postfix .. id:n) num:0) !! reserved-operator @2+2\n");
}
