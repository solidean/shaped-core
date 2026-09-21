#include "ast-test-support.hh"

#include <nexus/test.hh>

using sgl_test::ast_of;
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

TEST("sgl ast - a loop variable may carry its type")
{
    CHECK(body_of("for i : int in 0 ..< n:\n    total += i\n")
          == "(for i : int in (range ..< num:0 n)\n"
             "  (assign += total i))");
    CHECK(body_of("for _ : int in items => f()\n") == "(for _ : int in items => (call:paren f))");

    // The type belongs to one name, and a pattern is still no loop variable.
    CHECK(body_of("for (a, b) : t in ps:\n    f()\n").contains("for-takes-name-in-range"));
    CHECK(body_of("for i : int:\n    f()\n").contains("for-takes-name-in-range"));
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

TEST("sgl ast - no statement is exempt from no-effect, the last one of a block included")
{
    CHECK(body_of("let a = 1\na + 1\n") == "(let a = num:1)\n(call:infix + a num:1) !! no-effect @27+5\n");
    CHECK(body_of("if c:\n    a + 1\nf()\n").contains("no-effect"));

    // What has an effect: a paren or juxtaposition call, a jump, a `case`, a `loop`.
    CHECK(body_of("f(x)\nemit x\ncase k:\n    _ => g()\nloop:\n    break\nreturn a\n").contains("!!") == false);
    // What has none, whatever it is made of.
    CHECK(body_of("x.y\n") == "(member x y) !! no-effect @13+3\n");
    CHECK(body_of("(a, b)\n") == "(tuple a b) !! no-effect @13+6\n");
    CHECK(body_of("-x\n") == "(call:prefix - x) !! no-effect @13+2\n");
    CHECK(body_of("1.5\n") == "num:1.5 !! no-effect @13+3\n");
}

TEST("sgl ast - yield hands a value on from the nearest value block")
{
    // The block of a `case` arm, and the statement blocks in between are looked through.
    CHECK(body_of("let k = case kind:\n    .point =>:\n        if near:\n            yield 2.0\n        yield 1.0\n    "
                  "_ => 0.0\n")
          == "(let k = (case kind\n"
             "  (arm .point\n"
             "    (if\n"
             "      (branch near\n"
             "        (yield num:2.0)))\n"
             "    (yield num:1.0))\n"
             "  (arm _ => num:0.0)))");

    // The block of an arrow lambda, through a `for` and a `while`.
    CHECK(body_of("let g = x =>:\n    for i in r:\n        while c:\n            yield i\n    yield x\n")
          == "(let g = (lambda (params (field x))\n"
             "  (for i in r\n"
             "    (while c\n"
             "      (yield i)))\n"
             "  (yield x)))");

    // A `yield` takes the rest of a keyword form as its value, like `return`.
    CHECK(body_of("let g = x =>:\n    yield case x:\n        _ => 1\n")
          == "(let g = (lambda (params (field x))\n"
             "  (yield (case x\n"
             "    (arm _ => num:1)))))");
}

TEST("sgl ast - a yield directly in a function body is an error that keeps its node")
{
    CHECK(body_of("yield 1\n") == "(yield num:1) !! yield-in-function @13+5\n");
    CHECK(body_of("if c:\n    yield 1\n") == "(if\n  (branch c\n    (yield num:1))) !! yield-in-function @27+5\n");
    CHECK(ast_of("fun f() => yield 1\n") == "(fun f (params) => (yield num:1)) !! yield-in-function @11+5\n");

    // A function nested in a value block is a function again.
    CHECK(body_of("let g = x =>:\n    fun h():\n        yield 1\n    yield h()\n").contains("yield-in-function @56+5"));
    // A `loop:` in between changes nothing: there is still no value block to reach.
    CHECK(body_of("loop:\n    yield 1\n") == "(loop\n  (yield num:1)) !! yield-in-function @27+5\n");

    CHECK(body_of("let g = x =>:\n    yield\n")
          == "(let g = (lambda (params (field x))\n  (yield (invalid \"yield\")))) !! expected-expression @35+5\n");
    CHECK(body_of("let g = x =>:\n    yield a, b\n").contains("too-many-arguments"));

    auto const file = sgl::parse("fun f():\n    yield 1\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::yield_in_function);
    CHECK(ast.diagnostics[0].level == sgl::severity::normal_error);
}

TEST("sgl ast - a yield does not look through a loop, which is left with break")
{
    CHECK(body_of("let g = x =>:\n    loop:\n        yield x\n")
          == "(let g = (lambda (params (field x))\n"
             "  (loop\n"
             "    (yield x)))) !! yield-in-loop @53+5\n");
    // The `if` and the `for` on the way are transparent, and the `loop:` between them is what is reported.
    CHECK(body_of("let k = case kind:\n    _ =>:\n        loop:\n            for i in r:\n                "
                  "if c => yield i\n")
              .contains("yield-in-loop @120+5"));

    // What it is written as: `break` gives the `loop` its value, and the block hands that on.
    CHECK(body_of("let k = case kind:\n    _ =>:\n        yield loop:\n            break 1\n")
          == "(let k = (case kind\n"
             "  (arm _\n"
             "    (yield (loop\n"
             "      (break num:1))))))");
    // A value block inside the loop is reached without crossing it.
    CHECK(body_of("let r = loop:\n    let k = case kind:\n        _ =>:\n            yield 1\n    break k\n").contains("!!")
          == false);

    auto const file = sgl::parse("const g = x =>:\n    loop:\n        yield x\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::yield_in_loop);
    CHECK(ast.diagnostics[0].level == sgl::severity::normal_error);
}

TEST("sgl ast - a yield that is a whole one-line body is redundant, and its node is kept")
{
    CHECK(body_of("let k = case kind:\n    _ => yield 1\n")
          == "(let k = (case kind\n  (arm _ => (yield num:1)))) !! redundant-yield @45+5\n");
    CHECK(body_of("let g = x => yield x\n")
          == "(let g = (lambda (params (field x)) => (yield x))) !! redundant-yield @26+5\n");
    CHECK(ast_of("struct s:\n    length => yield w\n")
          == "(struct s\n  (property length => (yield w))) !! redundant-yield @24+5\n");
    // The keyword forms it takes as its value change nothing.
    CHECK(body_of("let g = x => yield case x:\n    _ => 1\n").contains("redundant-yield @26+5"));

    // A `yield` that is only part of the line is no body, and the statement right of `if … =>` is none either.
    CHECK(body_of("let g = x =>:\n    if c => yield 1\n    yield 2\n").contains("!!") == false);

    auto const file = sgl::parse("const g = x => yield x\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::redundant_yield);
    CHECK(ast.diagnostics[0].level == sgl::severity::warning);
}

TEST("sgl ast - a jump with nothing around it to leave")
{
    // A `yield` with neither a value block nor a `fun` around it.
    CHECK(ast_of("const k = yield 1\n") == "(const k = (yield num:1)) !! jump-without-target @10+5\n");
    CHECK(ast_of("const k = loop:\n    yield 1\n").contains("jump-without-target @20+5"));
    // A `return` looks through the property block, and a struct at file level has no `fun` around it.
    CHECK(ast_of("struct s:\n    area =>:\n        return 1\n")
          == "(struct s\n"
             "  (property area\n"
             "    (return num:1))) !! jump-without-target @31+6\n");
    CHECK(ast_of("const k = return 1\n") == "(const k = (return num:1)) !! jump-without-target @10+6\n");

    // `break` and `continue` look for a loop inside their function, through a `case` arm and an `if`.
    CHECK(body_of("break\n") == "(break) !! jump-without-target @13+5\n");
    CHECK(body_of("if c => continue\n") == "(if\n  (branch c => (continue))) !! jump-without-target @21+8\n");
    CHECK(body_of("for i in r:\n    case i:\n        0 => continue\n        _ => break\n").contains("!!") == false);
    CHECK(body_of("while c:\n    if d => break\n").contains("!!") == false);
    // A lambda is a function of its own, so the loop around it is out of reach.
    CHECK(body_of("for i in r:\n    let g = x =>:\n        break\n").contains("jump-without-target @59+5"));
    CHECK(body_of("loop:\n    let g = fun (x):\n        break\n").contains("jump-without-target @56+5"));

    auto const file = sgl::parse("const k = yield 1\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::jump_without_target);
    CHECK(ast.diagnostics[0].level == sgl::severity::normal_error);
}

TEST("sgl ast - a return that is a whole one-line arrow lambda body is redundant")
{
    CHECK(body_of("let g = x => return x\n")
          == "(let g = (lambda (params (field x)) => (return x))) !! redundant-return @26+6\n");
    CHECK(body_of("let g = x => return case x:\n    _ => 1\n").contains("redundant-return @26+6"));
    // A one-line arm is no lambda, and a `fun` is left with `return` however short it is.
    CHECK(body_of("let k = case kind:\n    _ => return 1\n").contains("!!") == false);
    CHECK(body_of("let g = fun (x) => return x\n").contains("!!") == false);

    auto const file = sgl::parse("const g = x => return x\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::redundant_return);
    CHECK(ast.diagnostics[0].level == sgl::severity::normal_error);
}

TEST("sgl ast - a return leaves the nearest fun, so the block of an arrow lambda has nothing to return from")
{
    CHECK(body_of("let g = x =>:\n    return x\n")
          == "(let g = (lambda (params (field x))\n  (return x))) !! return-in-lambda @35+6\n");
    // A `case` arm is looked through, and what it finds is the lambda.
    CHECK(body_of("let g = x => case x:\n    _ => return 1\n").contains("return-in-lambda @47+6"));

    // Through a `case` arm to the function around it.
    CHECK(body_of("let k = case kind:\n    _ =>:\n        return false\n")
          == "(let k = (case kind\n"
             "  (arm _\n"
             "    (return false))))");
    // An anonymous `fun` inside an arrow lambda is a `fun` to return from.
    CHECK(body_of("let g = x => fun (y):\n    return y\n")
          == "(let g = (lambda (params (field x)) => (lambda:fun (params (field y))\n"
             "  (return y))))");

    auto const file = sgl::parse("const g = x =>:\n    return x\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.diagnostics.size() == 1);
    CHECK(ast.diagnostics[0].kind == sgl::diagnostic_kind::return_in_lambda);
    CHECK(ast.diagnostics[0].level == sgl::severity::normal_error);
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
    CHECK(body_of("let x = 5 @range(0, 9)\n") == "(let{@range(num:0 num:9)} x = num:5)");
    CHECK(body_of("@a\nif c:\n    f()\n@b\nelse:\n    g()\n").contains("misplaced-attribute-on-expression"));
}

TEST("sgl ast - keywords that head nothing")
{
    CHECK(body_of("mut x\nf()\n") == "(invalid-stmt \"mut x\")\n(call:paren f) !! unexpected-keyword @13+3\n");
}
