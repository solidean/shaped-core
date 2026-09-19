#include "ast-test-support.hh"

#include <nexus/test.hh>

using sgl_test::ast_of;
using sgl_test::body_of;
using sgl_test::expr_of;

TEST("sgl ast - atoms")
{
    CHECK(expr_of("1.5e-3f32") == "num:1.5e-3f32");
    CHECK(expr_of("\"hi $name\"") == "str:\"hi $name\"");
    CHECK(expr_of("#ff00bb") == "hash:#ff00bb");
    CHECK(expr_of("x") == "x");
    CHECK(expr_of("_") == "_");
    CHECK(expr_of(".point") == ".point");
    CHECK(expr_of(".0") == ".0");
    CHECK(expr_of("a.b.c") == "(member (member a b) c)");
}

TEST("sgl ast - self is a reserved name and no keyword")
{
    CHECK(expr_of("self") == "self");
    CHECK(expr_of("self.x") == "(member self x)");

    // A keyword would head a keyword form and swallow the assignment.
    CHECK(body_of("self.x = 0\n") == "(assign = (member self x) num:0)");

    auto const file = sgl::parse("const r = self");
    auto const ast = sgl::ast::build(file);
    auto found = false;
    for (auto const& e : ast.exprs)
        found = found || e.node.is<sgl::ast::self_ref>();
    CHECK(found);
}

TEST("sgl ast - a call is one node with four spellings")
{
    CHECK(expr_of("f(x, y)") == "(call:paren f x y)");
    CHECK(expr_of("f x y") == "(call:juxt f x y)");
    CHECK(expr_of("a + b") == "(call:infix + a b)");
    CHECK(expr_of("-x") == "(call:prefix - x)");
    CHECK(expr_of("not x") == "(call:prefix not x)");
    CHECK(expr_of("f()") == "(call:paren f)");
    CHECK(expr_of("v.length()") == "(call:paren (member v length))");

    // A sign directly on a number is part of the literal.
    CHECK(expr_of("-3") == "num:-3");
}

TEST("sgl ast - the callee of an operator call is its token")
{
    auto const file = sgl::parse("const r = a + b");
    auto const ast = sgl::ast::build(file);
    auto count = 0;
    for (auto const& e : ast.exprs)
        if (auto const* c = e.node.try_as<sgl::ast::call>())
        {
            ++count;
            CHECK(c->spelling == sgl::ast::call_spelling::infix);
            CHECK(!sgl::ast::is_valid(c->callee));
            CHECK(file.text_of(file.at(c->op).where) == "+");
            CHECK(ast.at(c->arguments).size() == 2);
        }
    CHECK(count == 1);
}

TEST("sgl ast - a flat run nests to the left")
{
    CHECK(expr_of("a - b + c") == "(call:infix + (call:infix - a b) c)");
    CHECK(expr_of("a + b * c - d") == "(call:infix - (call:infix + a (call:infix * b c)) d)");
    CHECK(expr_of("a & b & c") == "(call:infix & (call:infix & a b) c)");
}

TEST("sgl ast - and / or are calls flagged short-circuiting")
{
    CHECK(expr_of("a and b") == "(call:infix:short-circuit and a b)");
    CHECK(expr_of("a or b or c") == "(call:infix:short-circuit or (call:infix:short-circuit or a b) c)");
    CHECK(expr_of("a and not b") == "(call:infix:short-circuit and a (call:prefix not b))");
}

TEST("sgl ast - one comparison is a call, two are a chain")
{
    CHECK(expr_of("a < b") == "(call:infix < a b)");
    CHECK(expr_of("0 <= i < n") == "(chain num:0 <= i < n)");
    CHECK(expr_of("a == b == c + 1") == "(chain a == b == (call:infix + c num:1))");

    auto const file = sgl::parse("const r = 0 <= i < n");
    auto const ast = sgl::ast::build(file);
    for (auto const& e : ast.exprs)
        if (auto const* chain = e.node.try_as<sgl::ast::comparison_chain>())
        {
            CHECK(ast.at(chain->operands).size() == 3);
            CHECK(ast.at(chain->operators).size() == 2);
        }
}

TEST("sgl ast - parentheses: (x) is x, (x,) is a tuple, () is the empty tuple")
{
    CHECK(expr_of("(x)") == "x");
    CHECK(expr_of("((x))") == "x");
    CHECK(expr_of("(x,)") == "(tuple x)");
    CHECK(expr_of("()") == "(tuple)");
    CHECK(expr_of("(a, b)") == "(tuple a b)");
    CHECK(expr_of("(a + b) * c") == "(call:infix * (call:infix + a b) c)");

    // A named or a splatted element is a list element, so the parentheses are a tuple.
    CHECK(expr_of("(x = 1)") == "(tuple x=num:1)");
    CHECK(expr_of("(..v)") == "(tuple ..v)");
}

TEST("sgl ast - arrays, and index as the neutral reading of a fused square list")
{
    CHECK(expr_of("[]") == "(array)");
    CHECK(expr_of("[1, 2]") == "(array num:1 num:2)");
    CHECK(expr_of("a[i]") == "(index a i)");
    CHECK(expr_of("texture2d[rgba8]") == "(index texture2d rgba8)");
    CHECK(expr_of("m[i, j].x") == "(member (index m i j) x)");
}

TEST("sgl ast - an assignment directly inside a paren list is a name")
{
    CHECK(expr_of("f(a, scale = 2)") == "(call:paren f a scale=num:2)");
    CHECK(expr_of("[x = 1]") == "(array x=num:1)");
    CHECK(expr_of("f(a.b = 2)") == "(call:paren f (invalid \"a.b = 2\")) !! expected-name @12+7\n");
    CHECK(expr_of("f(a += 2)") == "(call:paren f (invalid \"a += 2\")) !! statement-in-expression @12+6\n");
}

TEST("sgl ast - objects, and the shorthand is recorded and not expanded")
{
    CHECK(expr_of("{}") == "(object)");
    CHECK(expr_of("{a, b = 2}") == "(object a=<shorthand> b=num:2)");
    CHECK(expr_of("{..defaults, roughness = 0.5}") == "(object ..defaults roughness=num:0.5)");

    auto const file = sgl::parse("const r = {a}");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.arguments.size() == 1);
    CHECK(ast.arguments[0].is_shorthand);
    CHECK(!sgl::ast::is_valid(ast.arguments[0].value));
    CHECK(file.text_of(ast.arguments[0].name) == "a");
}

TEST("sgl ast - the splat is a whole element of a paren list")
{
    CHECK(expr_of("(..n, 0)") == "(tuple ..n num:0)");
    CHECK(expr_of("[..xs, 1]") == "(array ..xs num:1)");
    CHECK(expr_of("f(..args)") == "(call:paren f ..args)");
    CHECK(expr_of("a[..i]") == "(index a ..i)");
    CHECK(expr_of("(..f(x), 0)") == "(tuple ..(call:paren f x) num:0)");
}

TEST("sgl ast - a splat anywhere else is misplaced")
{
    CHECK(expr_of("..n") == "(invalid \"..n\") !! misplaced-splat @10+3\n");
    CHECK(expr_of("f ..n") == "(call:juxt f (invalid \"..n\")) !! misplaced-splat @12+3\n");
    CHECK(expr_of("(1 + ..n, 0)") == "(tuple (call:infix + num:1 (invalid \"..n\")) num:0) !! misplaced-splat @15+3\n");

    // The postfix spelling stays reserved, which the form parser has said already.
    CHECK(expr_of("(n.., 0)") == "(tuple (invalid \"n..\") num:0) !!syntax reserved-operator @12+2\n");
}

TEST("sgl ast - a curly list of name: type elements is a struct type")
{
    CHECK(expr_of("{pos: hpos4, normal: vec3}") == "(struct-type (field pos : hpos4) (field normal : vec3))");
    CHECK(expr_of("{@position pos: hpos4}") == "(struct-type (field{@position} pos : hpos4))");
    CHECK(expr_of("{k: float = 0.5}") == "(struct-type (field k : float = num:0.5))");
}

TEST("sgl ast - a curly list mixing fields and values is an error and reads as an object")
{
    CHECK(expr_of("{pos: hpos4, normal = n}") == "(object (ascribe pos : hpos4) normal=n) !! mixed-struct-type @10+24\n");
}

TEST("sgl ast - the type positions: cast, ascription, and membership beside them")
{
    CHECK(expr_of("x as mat3") == "(cast x : mat3)");
    CHECK(expr_of("x : int") == "(ascribe x : int)");
    CHECK(expr_of("x in 0 ..< 4") == "(in x (range ..< num:0 num:4))");
    CHECK(expr_of("x as int in r : bool") == "(ascribe (in (cast x : int) r) : bool)");
    CHECK(expr_of("(mvp as mat3) * v") == "(call:infix * (cast mvp : mat3) v)");
    CHECK(expr_of("x as buffer[float]") == "(cast x : (index buffer float))");
}

TEST("sgl ast - a range is a node of its own")
{
    CHECK(expr_of("0 ..< n") == "(range ..< num:0 n)");
    CHECK(expr_of("0..=n - 1") == "(range ..= num:0 (call:infix - n num:1))");
}

TEST("sgl ast - a function type binds tighter than the ascription around it")
{
    CHECK(expr_of("f : (int, int) -> int") == "(ascribe f : (function-type (params (field : int) (field : int)) -> int))");
    CHECK(expr_of("f : (x: int) -> int") == "(ascribe f : (function-type (params (field x : int)) -> int))");
    CHECK(expr_of("f : int -> int") == "(ascribe f : (function-type (params (field : int)) -> int))");
    CHECK(expr_of("f : () -> int") == "(ascribe f : (function-type (params) -> int))");
    CHECK(expr_of("f : a -> b -> c")
          == "(ascribe f : (function-type (params (field : (function-type (params (field : a)) -> b))) -> c))");
}

TEST("sgl ast - a fused curly list on an expression is reserved")
{
    CHECK(expr_of("f(x){frame}") == "(with-bindings (call:paren f x) frame=<shorthand>) !! unsupported-syntax @14+7\n");
}

TEST("sgl ast - lambdas")
{
    CHECK(expr_of("x => x + 1") == "(lambda (params (field x)) => (call:infix + x num:1))");
    CHECK(expr_of("_ => 0") == "(lambda (params (field _)) => num:0)");
    CHECK(expr_of("(a, b: int) => a") == "(lambda (params (field a) (field b : int)) => a)");
    CHECK(expr_of("() => 1") == "(lambda (params) => num:1)");
    CHECK(expr_of("x => y => x") == "(lambda (params (field x)) => (lambda (params (field y)) => x))");
    CHECK(expr_of("f(1) => x") == "(invalid \"f(1) => x\") !! expected-parameter @10+9\n");
    CHECK(expr_of("(a + 1) => x") == "(lambda (params (field : (invalid \"a + 1\"))) => x) !! expected-parameter @11+5\n");

    CHECK(ast_of("const r = on_click(handler = e =>:\n    print e\n)\n")
          == "(const r = (call:paren on_click handler=(lambda (params (field e))\n"
             "  (print e))))");
}

TEST("sgl ast - case with expression arms, block arms and a jump as an arm")
{
    CHECK(body_of("let k = case kind:\n    .point => 1.0\n    .spot or .area => 2.0\n    _ => return false\n")
          == "(let k = (case kind\n"
             "  (arm .point => num:1.0)\n"
             "  (arm (call:infix:short-circuit or .spot .area) => num:2.0)\n"
             "  (arm _ => (return false))))");

    CHECK(body_of("let k = case kind:\n    .point =>:\n        let a = 1\n        a\n    _ => 0\n")
          == "(let k = (case kind\n"
             "  (arm .point\n"
             "    (let a = num:1)\n"
             "    a)\n"
             "  (arm _ => num:0)))");
}

TEST("sgl ast - what is no arm in a case block, and a case without its parts")
{
    CHECK(body_of("case k:\n    .a => 1\n    f(x)\n")
          == "(case k\n"
             "  (arm .a => num:1)\n"
             "  (arm <missing> => (invalid \"f(x)\")))"
             " !! expected-case-arm @45+4\n");
    CHECK(body_of("case k\n") == "(case k) !! expected-body @13+4\n");
    CHECK(body_of("case:\n    _ => 1\n")
          == "(case (invalid \"case:\")\n  (arm _ => num:1)) !! expected-expression @13+4\n");
}

TEST("sgl ast - loop yields through break, and the jumps are expressions")
{
    CHECK(body_of("let r = loop:\n    if done => break 5\n    continue\n")
          == "(let r = (loop\n"
             "  (if\n"
             "    (branch done => (break num:5)))\n"
             "  (continue)))");
    CHECK(body_of("return\n") == "(return)");
    CHECK(body_of("return {a = 1}\n") == "(return (object a=num:1))");
    CHECK(body_of("return a, b\n") == "(return a) !! too-many-arguments @23+1\n");
    CHECK(body_of("continue x\n") == "(continue) !! too-many-arguments @22+1\n");
    CHECK(body_of("loop\n") == "(loop) !! expected-body @13+4\n");
}

TEST("sgl ast - a statement where a value is expected")
{
    CHECK(expr_of("(a = b) + 1") == "(call:infix + (tuple a=b) num:1)");
    CHECK(expr_of("f(let x)") == "(call:paren f (invalid \"let x\")) !! statement-in-expression @12+3\n");
    CHECK(expr_of("x = y") == "(invalid \"x = y\") !! statement-in-expression @10+5\n");
    CHECK(expr_of("f(mut x)") == "(call:paren f (invalid \"mut x\")) !! unexpected-keyword @12+3\n");
}

TEST("sgl ast - what the form parser could not read is invalid without a second diagnostic")
{
    CHECK(expr_of("a +") == "(call:infix + a (invalid \"\")) !!syntax expected-expression @12+1\n");
    CHECK(expr_of("x!") == "(invalid \"x!\") !!syntax reserved-operator @11+1\n");
}

TEST("sgl ast - an expression that owns a block has no reading yet")
{
    CHECK(body_of("f(x):\n    y\n") == "(invalid \"f(x):\") !! unsupported-syntax @13+4\n");
}

TEST("sgl ast - an attribute on a sub-expression is rejected and kept")
{
    CHECK(expr_of("f(@hot x)") == "(call:paren f x{@hot}) !! misplaced-attribute-on-expression @17+1\n");
    CHECK(expr_of("x in @a r") == "(in x r{@a}) !! misplaced-attribute-on-expression @18+1\n");

    // A type position takes them.
    CHECK(expr_of("x as @unorm vec4") == "(cast x : vec4{@unorm})");
    CHECK(expr_of("x : @a(1, 2) t") == "(ascribe x : t{@a(1, 2)})");
}

TEST("sgl ast - consecutive keywords head one form, and a jump takes the rest as its value")
{
    // The forms hold `(kw kw:return kw:case id:k …)`, one keyword form with two keywords.
    CHECK(body_of("return case k:\n    .a => 1\n    _ => 2\n")
          == "(return (case k\n"
             "  (arm .a => num:1)\n"
             "  (arm _ => num:2)))");
    CHECK(body_of("let r = loop:\n    break case k:\n        _ => 1\n")
          == "(let r = (loop\n"
             "  (break (case k\n"
             "    (arm _ => num:1)))))");
    CHECK(body_of("return return\n") == "(return (return))");
    CHECK(body_of("return let x\n") == "(return (invalid \"return let x\")) !! statement-in-expression @13+6\n");
    CHECK(body_of("continue break\n").contains("unexpected-keyword"));
}
