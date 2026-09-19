#include "ast-test-support.hh"

#include <nexus/test.hh>

using sgl_test::body_of;

// `body_of` reads its lines as the body of `fun f():`, so a diagnostic offset counts that line (9 bytes) and the
// four spaces of indentation in front of each statement.

TEST("sgl ast - let binds a pattern, optionally typed, optionally initialized")
{
    CHECK(body_of("let x = 10\n") == "(let x = num:10)");
    CHECK(body_of("let x : int\n") == "(let x : int)");
    CHECK(body_of("let x : int = 10\n") == "(let x : int = num:10)");
    CHECK(body_of("let mut color = m.albedo * k\n") == "(let mut color = (call:infix * (member m albedo) k))");
    CHECK(body_of("let _ = f()\n") == "(let _ = (call:paren f))");
    CHECK(body_of("let f : (int) -> int = g\n") == "(let f : (function-type (params (field : int)) -> int) = g)");
}

TEST("sgl ast - a let pattern is a name, a wildcard, or a round list of patterns")
{
    CHECK(body_of("let (a, b) = pair\n") == "(let (tuple a b) = pair)");
    CHECK(body_of("let mut (a, b) : t = pair\n") == "(let mut (tuple a b) : t = pair)");
    CHECK(body_of("let (a, (_, c)) = nested\n") == "(let (tuple a (tuple _ c)) = nested)");

    CHECK(body_of("let a.b = 1\n") == "(let (invalid \"a.b\") = num:1) !! expected-pattern @17+3\n");
    CHECK(body_of("let (a, 1) = p\n") == "(let (invalid \"(a, 1)\") = p) !! expected-pattern @17+6\n");
    CHECK(body_of("let = 5\n") == "(let (invalid \"let\") = num:5) !! expected-pattern @13+3\n");
    CHECK(body_of("let x += 5\n") == "(let x = num:5) !! unexpected-token @19+2\n");
    CHECK(body_of("let mut mut x = 5\n") == "(invalid-stmt \"let mut mut x = 5\") !! unexpected-keyword @17+3\n");
}

TEST("sgl ast - assignment and compound assignment are statements")
{
    CHECK(body_of("x = 1\n") == "(assign = x num:1)");
    CHECK(body_of("total += i * 2\n") == "(assign += total (call:infix * i num:2))");
    CHECK(body_of("v.x[0] <<= 1\n") == "(assign <<= (index (member v x) num:0) num:1)");
    CHECK(body_of("(a, b) = (b, a)\n") == "(assign = (tuple a b) (tuple b a))");

    // An assignment is no value, so a chained one has nothing to assign.
    CHECK(body_of("a = b = c\n") == "(assign = a (invalid \"b = c\")) !! statement-in-expression @17+5\n");
}

TEST("sgl ast - sibling if / else if / else forms are one chain")
{
    CHECK(body_of("if a:\n    f()\nelse if b:\n    g()\nelse:\n    h()\n")
          == "(if\n"
             "  (branch a\n"
             "    (call:paren f))\n"
             "  (branch b\n"
             "    (call:paren g))\n"
             "  (else\n"
             "    (call:paren h)))");

    // Blank lines and comments between the siblings are no forms.
    CHECK(body_of("if a:\n    f()\n\n// otherwise\nelse:\n    h()\n")
          == "(if\n"
             "  (branch a\n"
             "    (call:paren f))\n"
             "  (else\n"
             "    (call:paren h)))");

    auto const file = sgl::parse("fun f():\n    if a:\n        g()\n    else:\n        h()\n");
    auto const ast = sgl::ast::build(file);
    CHECK(ast.if_branches.size() == 2);
    auto if_count = 0;
    for (auto const& s : ast.stmts)
        if_count += s.node.is<sgl::ast::if_stmt>() ? 1 : 0;
    CHECK(if_count == 1);
}

TEST("sgl ast - the one-line form of if takes a statement right of the arrow")
{
    CHECK(body_of("if done => return\n") == "(if\n  (branch done => (return)))");
    CHECK(body_of("if a => f()\nelse if b => g()\nelse => h()\n")
          == "(if\n"
             "  (branch a => (call:paren f))\n"
             "  (branch b => (call:paren g))\n"
             "  (else => (call:paren h)))");

    // `=` is looser than `=>`, so the forms hold `((if done) => total) = 0`; the AST puts it back together.
    CHECK(body_of("if done => total = 0\n") == "(if\n  (branch done => (assign = total num:0)))");
    CHECK(body_of("if a => if b => f()\n") == "(if\n  (branch a => (if\n    (branch b => (call:paren f)))))");
}

TEST("sgl ast - an else chain ends at a plain else, and an else without an if is stray")
{
    CHECK(body_of("if a:\n    f()\nelse:\n    g()\nelse:\n    h()\n")
          == "(if\n"
             "  (branch a\n"
             "    (call:paren f))\n"
             "  (else\n"
             "    (call:paren g)))\n"
             "(if\n"
             "  (else\n"
             "    (call:paren h)))"
             " !! stray-else @57+4\n");

    // Its body is still read, and an `else if` keeps its condition.
    CHECK(body_of("else:\n    let x = 1\n") == "(if\n  (else\n    (let x = num:1))) !! stray-else @13+4\n");
    CHECK(body_of("else if c:\n    f()\nelse:\n    g()\n")
          == "(if\n"
             "  (branch c\n"
             "    (call:paren f))\n"
             "  (else\n"
             "    (call:paren g)))"
             " !! stray-else @13+4\n");

    // Anything between them breaks the pair.
    CHECK(body_of("if a:\n    f()\ng()\nelse:\n    h()\n").contains("stray-else"));
}

TEST("sgl ast - an if without its parts")
{
    CHECK(body_of("if:\n    f()\n")
          == "(if\n  (branch (invalid \"if:\")\n    (call:paren f))) !! expected-expression @13+2\n");
    CHECK(body_of("if a\n") == "(if\n  (branch a)) !! expected-body @13+2\n");
    CHECK(body_of("if a, b:\n    f()\n") == "(if\n  (branch a\n    (call:paren f))) !! too-many-arguments @19+1\n");
    CHECK(body_of("if a:\n    f()\nelse b:\n    g()\n").contains("too-many-arguments"));
}

TEST("sgl ast - for takes exactly name in range")
{
    CHECK(body_of("for i in 0 ..< n:\n    total += i\n")
          == "(for i in (range ..< num:0 n)\n"
             "  (assign += total i))");
    CHECK(body_of("for _ in items => f()\n") == "(for _ in items => (call:paren f))");

    CHECK(body_of("for i:\n    f()\n") == "(for <none> in i\n  (call:paren f)) !! for-takes-name-in-range @13+3\n");
    CHECK(body_of("for (a, b) in ps:\n    f()\n")
          == "(for <none> in (in (tuple a b) ps)\n  (call:paren f)) !! for-takes-name-in-range @13+3\n");
    CHECK(body_of("for i in a in b:\n    f()\n").contains("for-takes-name-in-range"));
    CHECK(body_of("for i in r\n") == "(for i in r) !! expected-body @13+3\n");
}

TEST("sgl ast - while")
{
    CHECK(body_of("while i < n:\n    i += 1\n") == "(while (call:infix < i n)\n  (assign += i num:1))");
    CHECK(body_of("while:\n    f()\n") == "(while (invalid \"while:\")\n  (call:paren f)) !! expected-expression @13+5\n");
    CHECK(body_of("while a, b:\n    f()\n").contains("too-many-arguments"));
}

TEST("sgl ast - assert takes a condition and at most a message")
{
    CHECK(body_of("assert a == b\n") == "(assert (call:infix == a b))");
    CHECK(body_of("assert a == b, \"sizes differ: $a\"\n") == "(assert (call:infix == a b) str:\"sizes differ: $a\")");

    CHECK(body_of("assert\n") == "(assert <none>) !! assert-takes-condition-and-message @13+6\n");
    CHECK(body_of("assert a, \"m\", extra\n") == "(assert a str:\"m\") !! assert-takes-condition-and-message @13+6\n");
}

TEST("sgl ast - print takes exactly one message")
{
    CHECK(body_of("print \"total: $total\"\n") == "(print str:\"total: $total\")");
    CHECK(body_of("print\n") == "(print <none>) !! print-takes-one-message @13+5\n");
    CHECK(body_of("print \"total:\", total\n") == "(print str:\"total:\") !! print-takes-one-message @13+5\n");
}

TEST("sgl ast - an expression statement is a call or a jump, anything else has no effect")
{
    CHECK(body_of("f(x)\n") == "(call:paren f x)");
    CHECK(body_of("emit x\n") == "(call:juxt emit x)");
    CHECK(body_of("return x\n") == "(return x)");
    CHECK(body_of("loop:\n    break\n") == "(loop\n  (break))");

    CHECK(body_of("x\nf()\n") == "x\n(call:paren f) !! no-effect @13+1\n");
    CHECK(body_of("a + b\nf()\n") == "(call:infix + a b)\n(call:paren f) !! no-effect @13+5\n");
    CHECK(body_of("x => x\nf()\n").contains("no-effect"));

    // A warning: the statement is legal and almost certainly not what was meant.
    auto const file = sgl::parse("fun f():\n    x\n    g()\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::no_effect);
    CHECK(ast.diagnostics[0].level == sgl::severity::warning);
}

TEST("sgl ast - the last statement of a block that yields a value is what it computes")
{
    CHECK(body_of("let a = 1\na + 1\n") == "(let a = num:1)\n(call:infix + a num:1)");

    // A control statement's block yields nothing.
    CHECK(body_of("if c:\n    a + 1\nf()\n").contains("no-effect"));
}

TEST("sgl ast - statements separated by a semicolon are statements of their own")
{
    CHECK(body_of("let a = 1; f(a)\n") == "(let a = num:1)\n(call:paren f a)");
}

TEST("sgl ast - an attribute on a statement is kept on it")
{
    CHECK(body_of("@unroll\nfor i in 0 ..< 4:\n    f(i)\n")
          == "(for{@unroll} i in (range ..< num:0 num:4)\n  (call:paren f i))");
    CHECK(body_of("@hot f(x)\ng()\n") == "(call:paren f x){@hot}\n(call:paren g)");
    CHECK(body_of("let x = 5 @range(0, 9)\n") == "(let{@range(0, 9)} x = num:5)");
    CHECK(body_of("@a\nif c:\n    f()\n@b\nelse:\n    g()\n").contains("misplaced-attribute-on-expression"));
}

TEST("sgl ast - keywords that head nothing")
{
    CHECK(body_of("mut x\nf()\n") == "(invalid-stmt \"mut x\")\n(call:paren f) !! unexpected-keyword @13+3\n");
}
