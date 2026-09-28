#include "source-flat-test-support.hh"

using namespace sgl_test;

namespace
{
/// The reports of `lines` as the body of `fun f(k: float, n: int) -> float`; `lines` is indented here.
cc::string body_reports(cc::string_view lines)
{
    auto source = cc::string("fun f(k: float, n: int) -> float:\n");
    auto at_line_start = true;
    for (auto const ch : lines)
    {
        if (at_line_start && ch != '\n')
            source += "    ";
        source += ch;
        at_line_start = ch == '\n';
    }
    return reports_for(source);
}
} // namespace

// ---- what the check pass says -----------------------------------------------------------------------------------------

TEST("sgl check - if, while, for and loop check clean, and a condition is a bool")
{
    CHECK(body_reports("if k > 1.0:\n    return 1.0\nelse if k > 0.5:\n    return 0.5\nelse:\n    return 0.0\n") == "");
    CHECK(body_reports("if k > 1.0 => return 1.0\nreturn k\n") == "");
    CHECK(body_reports("let mut x = k\nwhile x < 8.0:\n    x = x * 2.0\nreturn x\n") == "");
    CHECK(body_reports("let mut x = k\nfor i in 0 ..< n:\n    x += 1.0\nreturn x\n") == "");
    CHECK(body_reports("let mut x = k\nfor i : int in 0 ..< n:\n    x += 1.0\nreturn x\n") == "");
    CHECK(body_reports("let mut x = k\nloop:\n    x *= 2.0\n    if x > 8.0 => break\nreturn x\n") == "");

    CHECK(body_reports("if k => return 1.0\nreturn k\n") == "type-mismatch user:[k] a condition is a bool, got float\n");
    CHECK(body_reports("while n:\n    return 1.0\nreturn k\n")
          == "type-mismatch user:[n] a condition is a bool, got int\n");
    CHECK(body_reports("for i in 0 ..< k:\n    return 1.0\nreturn k\n")
          == "type-mismatch user:[k] a for runs over int, got float\n");
    CHECK(body_reports("for i : float in 0 ..< n:\n    return 1.0\nreturn k\n")
          == "type-mismatch user:[float] a for runs over int, got float\n");
}

TEST("sgl check - and, or and not are the language's own, and take bools")
{
    CHECK(body_reports("if k > 0.0 and k < 1.0 or not (n == 0) => return 1.0\nreturn k\n") == "");
    CHECK(body_reports("if k and k < 1.0 => return 1.0\nreturn k\n")
          == "no-matching-overload user:[k and k < 1.0] operator and(float, bool)\n");
    CHECK(body_reports("if not n => return 1.0\nreturn k\n") == "no-matching-overload user:[not n] operator not(int)\n");
}

TEST("sgl check - a comparison chain resolves each operator")
{
    CHECK(body_reports("if 0.0 <= k < 1.0 => return 1.0\nreturn k\n") == "");
    CHECK(body_reports("if 0 <= n < 8 <= n => return 1.0\nreturn k\n") == "");
    CHECK(body_reports("if 0.0 <= k < n => return 1.0\nreturn k\n")
          == "no-matching-overload user:[0.0 <= k < n] operator <(float, int)\n");
}

TEST("sgl check - an assignment names a mutable local, or a member of one")
{
    CHECK(body_reports("let mut v = vec3(k, k, k)\nv.x = 1.0\nv = normalize v\nreturn v.x\n") == "");
    CHECK(body_reports("let v = k\nv = 1.0\nreturn v\n")
          == "not-assignable user:[v] v is immutable; `let mut` declares a local an assignment may name\n");
    CHECK(body_reports("k = 1.0\nreturn k\n") == "not-assignable user:[k] k is a parameter, which is a value\n");
    CHECK(body_reports("for i in 0 ..< n:\n    i = 2\nreturn k\n")
          == "not-assignable user:[i] i is immutable; `let mut` declares a local an assignment may name\n");
    CHECK(body_reports("let mut x = k\nx = n\nreturn x\n") == "type-mismatch user:[n] expected float, got int\n");
    CHECK(body_reports("let mut x = k\nx += n\nreturn x\n")
          == "no-matching-overload user:[x += n] operator +(float, int)\n");
    CHECK(body_reports("let mut b = k < 1.0\nb -= b\nreturn k\n")
          == "no-matching-overload user:[b -= b] operator -(bool, bool)\n");
    CHECK(reports_for("binding frame:\n    exposure: float\nfun f(k: float){frame} -> float:\n    frame.exposure = k\n"
                      "    return k\n")
          == "not-assignable user:[frame.exposure] frame is a binding, which no shader writes\n");
}

TEST("sgl check - a block is a scope: a local ends with it, and a later local shadows an earlier one")
{
    CHECK(body_reports("if k > 0.0:\n    let x = k\nreturn x\n").starts_with("unknown-name user:[x] x\n"));
    // two blocks beside each other share nothing
    CHECK(body_reports("if k > 0.0:\n    let x = 1.0\n    return x\nelse:\n    let x = 2.0\n    return x\n") == "");
    // CHK-53: in the same block, in an inner one, over a parameter and by a `for`, as in Rust
    CHECK(body_reports("let x = k\nlet x = x * 2.0\nreturn x\n") == "");
    CHECK(body_reports("let x = k\nif k > 0.0:\n    let x = 1.0\nreturn x\n") == "");
    CHECK(body_reports("let k = k * 2.0\nreturn k\n") == "");
    CHECK(body_reports("for k in 0 ..< n:\n    return 1.0\nreturn 0.0\n") == "");
    // a shadowing local may change the type, and the value it is given still sees the one it hides
    CHECK(body_reports("let x = n\nlet x = k\nreturn x\n") == "");
    CHECK(body_reports("let x = k\nlet x = x < 1.0\nreturn x\n") == "type-mismatch user:[x] expected float, got bool\n");
}

TEST("sgl check - every path of a function that returns a value ends in a return")
{
    CHECK(body_reports("let x = k\n")
          == "missing-return user:[f] f returns float, and a path through its body ends without a return\n");
    CHECK(body_reports("if k > 0.0 => return 1.0\n").starts_with("missing-return user:[f]"));
    CHECK(body_reports("if k > 0.0:\n    return 1.0\nelse if k < -1.0:\n    return 2.0\n")
              .starts_with("missing-return user:[f]"));
    CHECK(body_reports("if k > 0.0:\n    return 1.0\nelse:\n    return 2.0\n") == "");

    // A `loop:` nothing breaks out of never ends, so nothing falls off behind it.
    CHECK(body_reports("let mut x = k\nloop:\n    x *= 2.0\n    if x > 8.0 => return x\n") == "");
    CHECK(body_reports("let mut x = k\nloop:\n    x *= 2.0\n    if x > 8.0 => break\n")
              .starts_with("missing-return user:[f]"));
    // A `while` ends when its condition says so, whatever the condition is.
    CHECK(body_reports("while 0 < 1:\n    return 1.0\n").starts_with("missing-return user:[f]"));
    CHECK(body_reports("for i in 0 ..< n:\n    return 1.0\n").starts_with("missing-return user:[f]"));
}

TEST("sgl check - a function without a return type returns nothing")
{
    CHECK(reports_for("fun note(x: float):\n    if x < 0.0 => return\n    print x\n") == "");
    CHECK(reports_for("fun note(x: float):\n    return x\n")
          == "type-mismatch user:[x] note has no return type, so its return has no value\n");
    CHECK(reports_for("fun half(x: float) -> float:\n    return\n")
          == "type-mismatch user:[return] half returns float, and this return has no value\n");
    CHECK(reports_for("fun note(x: float):\n    print x\nfun f(k: float) -> float:\n    let y = note k\n    return k\n")
          == "type-mismatch user:[note k] a let takes a value, and this is nothing\n");
    CHECK(reports_for("fun note(x: float):\n    print x\nfun f(k: float) -> float:\n    note k\n    return k\n") == "");
}

TEST("sgl check - an arrow body without a return type returns what its expression is")
{
    auto const checked = check_sources(read_prelude(), "fun half(x: float) => x * 0.5\n"
                                                       "fun f(k: float) -> float:\n"
                                                       "    return half(k) + half k\n");
    CHECK(reports_of(checked) == "");
    CHECK(sgl::check::dump(checked.module).contains("(fun half (x : float) -> float)\n"));

    // declared below its caller, and through a chain of inferred functions: the body is part of what a caller demands
    CHECK(reports_for("fun f(k: float) -> float => outer k\n"
                      "fun outer(x: float) => inner(x) * 2.0\n"
                      "fun inner(x: float) => x + 1.0\n")
          == "");
    // what it infers is what a caller has to fit
    CHECK(reports_for("fun flag(x: float) => x < 0.5\nfun f(k: float) -> float:\n    return flag k\n")
          == "type-mismatch user:[flag k] expected float, got bool\n");
    // a BLOCK body without `-> T` still returns nothing
    CHECK(reports_for("fun note(x: float):\n    return x * 0.5\n")
          == "type-mismatch user:[x * 0.5] note has no return type, so its return has no value\n");
    // no value, no result
    CHECK(reports_for("fun note(x: float):\n    print x\nfun relay(x: float) => note x\n")
          == "type-mismatch user:[note x] the body of relay is its result, and this is nothing\n");
    // a body that does not check is one report, and the caller of the function says nothing
    CHECK(reports_for("fun broken(x: float) => x + missing\nfun f(k: float) -> float:\n    return broken k\n")
          == "unknown-name user:[missing] missing\n");
}

TEST("sgl check - an inferred result that needs itself is a dependency cycle")
{
    // Inference makes the body part of the signature, so this is no recursive call: there is no signature to call yet.
    CHECK(reports_for("fun again(x: float) => again(x)\n") == "dependency-cycle user:[again(x)] again -> again\n");
    CHECK(reports_for("fun ping(x: float) => pong(x)\nfun pong(x: float) => ping(x)\n")
          == "dependency-cycle user:[ping(x)] ping -> pong -> ping\n");

    // One written return type breaks the cycle of signatures, and what is left is the recursion it always was.
    CHECK(reports_for("fun ping(x: float) => pong(x)\nfun pong(x: float) -> float => ping(x)\n")
          == "recursive-call user:[ping(x)] ping -> pong -> ping\n");

    // An overload that could never be chosen is not needed: its parameters are known before its result is.
    CHECK(reports_for("fun twice(x: float) => x * 2.0\n"
                      "fun twice(v: vec3) => v * twice(1.0)\n"
                      "fun f(v: vec3) -> vec3 => twice v\n")
          == "");

    // Every symbol settles, whichever of them was demanded first.
    auto const checked = check_sources(read_prelude(), "fun f(k: float) -> float => ping k\n"
                                                       "fun ping(x: float) => pong(x)\n"
                                                       "fun pong(x: float) => ping(x)\n");
    for (auto const& s : checked.module.symbols)
        CHECK((s.state == sgl::check::symbol_state::checked || s.state == sgl::check::symbol_state::failed));
}

TEST("sgl check - what follows a jump is reported once, as a warning")
{
    auto const checked = check_sources(read_prelude(), "fun f(k: float) -> float:\n"
                                                       "    return k\n"
                                                       "    let a = k\n"
                                                       "    let b = k\n");
    CHECK(reports_of(checked) == "unreachable-code user:[let a = k]\n");
    REQUIRE(checked.module.diagnostics.size() == 1);
    CHECK(checked.module.diagnostics[0].what.level == sgl::severity::warning);

    CHECK(body_reports("for i in 0 ..< n:\n    continue\n    print i\nreturn k\n") == "unreachable-code user:[print i]\n");
    CHECK(body_reports("loop:\n    return k\nreturn k\n") == "unreachable-code user:[return k]\n");
}

TEST("sgl check - a loop that is a value is left with break value, and any other with a bare break")
{
    CHECK(body_reports("let mut x = k\nlet y = loop:\n    x *= 2.0\n    if x > 8.0 => break x\nreturn y\n") == "");
    CHECK(body_reports("let mut x = k\nlet y = loop:\n    x *= 2.0\n    if x > 8.0 => break\nreturn x\n")
          == "type-mismatch user:[break] this loop is a value, so its break carries one\n");
    CHECK(body_reports("let mut x = k\nlet y = loop:\n    if x > 8.0 => break x\n    if x < 0.0 => break n\nreturn y\n")
          == "type-mismatch user:[n] an earlier break of this loop carries float, got int\n");
    CHECK(body_reports("let mut x = k\nloop:\n    x *= 2.0\n    if x > 8.0 => break x\nreturn x\n")
          == "type-mismatch user:[x] nothing reads the value of this loop, so its break carries none\n");
    CHECK(body_reports("let y = loop:\n    return k\nreturn y\n")
              .contains("unsupported-yet user:[loop:] a loop without a break as a value\n"));
}

TEST("sgl check - recursion is an error that names the loop, once")
{
    CHECK(reports_for("fun f(k: float) -> float:\n    return f(k)\n") == "recursive-call user:[f(k)] f -> f\n");
    CHECK(reports_for("fun a(k: float) -> float => b(k)\n"
                      "fun b(k: float) -> float => c(k) + c(k)\n"
                      "fun c(k: float) -> float:\n"
                      "    if k < 0.0 => return 0.0\n"
                      "    return a(k - 1.0)\n"
                      "fun outside(k: float) -> float => a(k)\n")
          == "recursive-call user:[a(k - 1.0)] a -> b -> c -> a\n");
}

TEST("sgl check - an entry point that reaches recursion has no flat tree")
{
    auto const checked = check_program(cc::string("fun down(k: float) -> float:\n"
                                                  "    if k < 0.0 => return 0.0\n"
                                                  "    return down(k - 1.0)\n")
                                       + grey_source("", "let x = down p.a\n"));
    CHECK(reports_of(checked) == "recursive-call user:[down(k - 1.0)] down -> down\n");
    CHECK(checked.module.entry_points.empty());
}

TEST("sgl check - bindings are an effect: a caller lists what its callees read")
{
    auto const head = cc::string("binding frame:\n    exposure: float\n"
                                 "fun exposed(k: float){frame} -> float => k * frame.exposure\n");
    CHECK(reports_for(head + "fun f(k: float){frame} -> float => exposed k\n") == "");
    CHECK(reports_for(head + "fun f(k: float) -> float => exposed k\n")
          == "binding-not-listed user:[exposed k] exposed needs frame, which is not in the binding list of f\n");
    // transitively: each caller is asked for its callee's list, up to whoever starts the chain
    CHECK(reports_for(head + "fun middle(k: float){frame} -> float => exposed k\nfun f(k: float) -> float => middle k\n")
          == "binding-not-listed user:[middle k] middle needs frame, which is not in the binding list of f\n");
}

TEST("sgl check - a function nobody calls is checked, and one called twice reports once")
{
    CHECK(reports_for("fun unused(k: float) -> float => nope\n") == "unknown-name user:[nope] nope\n");
    CHECK(reports_for("fun broken(k: float) -> float => nope\n"
                      "fun f(k: float) -> float => broken(k) + broken(k) + broken(1.0)\n")
          == "unknown-name user:[nope] nope\n");
}

// ---- the structured form --------------------------------------------------------------------------------------------

TEST("sgl check - an if chain is nested ifs, and a one-line branch is a branch like any other")
{
    CHECK(structured_dump("", "let mut x = 0.0\n"
                              "if p.a < 0.25:\n"
                              "    x = 1.0\n"
                              "else if p.a < 0.5 => x = 2.0\n"
                              "else:\n"
                              "    x = 3.0\n")
          == "  (var x : float = (lit 0.0 : float))\n"
             "  (if (call less (member (local p : frag) a : float) (lit 0.25 : float) : bool)\n"
             "    (then\n"
             "      (assign (local x : float) = (lit 1.0 : float)))\n"
             "    (else\n"
             "      (if (call less (member (local p : frag) a : float) (lit 0.5 : float) : bool)\n"
             "        (then\n"
             "          (assign (local x : float) = (lit 2.0 : float)))\n"
             "        (else\n"
             "          (assign (local x : float) = (lit 3.0 : float))))))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - while, for and loop keep their shape, and break and continue name their loop")
{
    CHECK(structured_dump("", "let mut x = p.a\n"
                              "while x < 8.0:\n"
                              "    x *= 2.0\n"
                              "for i in 0 ..< 4:\n"
                              "    if i == 2 => continue\n"
                              "    x += 1.0\n"
                              "loop:\n"
                              "    x -= 3.0\n"
                              "    if x < 0.0 => break\n")
          == "  (var x : float = (member (local p : frag) a : float))\n"
             "  (while $while (call less (local x : float) (lit 8.0 : float) : bool)\n"
             "    (assign (local x : float) = (call multiply (local x : float) (lit 2.0 : float) : float)))\n"
             "  (for $for i in (lit 0 : int) ..< (lit 4 : int)\n"
             "    (if (call equal_int (local i : int) (lit 2 : int) : bool)\n"
             "      (then\n"
             "        (continue $for)))\n"
             "    (assign (local x : float) = (call add (local x : float) (lit 1.0 : float) : float)))\n"
             "  (loop $loop\n"
             "    (assign (local x : float) = (call subtract (local x : float) (lit 3.0 : float) : float))\n"
             "    (if (call less (local x : float) (lit 0.0 : float) : bool)\n"
             "      (then\n"
             "        (leave $loop))))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - a loop that is a value is a block around the loop")
{
    CHECK(structured_dump("", "let mut w = p.a\n"
                              "let x = loop:\n"
                              "    w *= 2.0\n"
                              "    if w > 4.0 => break w\n")
          == "  (var w : float = (member (local p : frag) a : float))\n"
             "  (let x : float = (block $loop_value\n"
             "      (loop $loop\n"
             "        (assign (local w : float) = (call multiply (local w : float) (lit 2.0 : float) : float))\n"
             "        (if (call greater (local w : float) (lit 4.0 : float) : bool)\n"
             "          (then\n"
             "            (leave $loop_value (local w : float))))) : float))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - and, or and not are nodes of their own, and a chain evaluates its middle once")
{
    CHECK(structured_dump("", "let mut x = 0.0\n"
                              "if p.a < 0.5 and not (p.b < 0.5) or p.a == p.b => x = 1.0\n"
                              "if 0.0 <= p.a * 2.0 < 1.0 => x = 2.0\n"
                              "if 0.0 <= x < 1.0 => x = 3.0\n")
          == "  (var x : float = (lit 0.0 : float))\n"
             "  (if (or (and (call less (member (local p : frag) a : float) (lit 0.5 : float) : bool) (not (call less "
             "(member (local p : frag) b : float) (lit 0.5 : float) : bool) : bool) : bool) (call equal (member (local "
             "p : frag) a : float) (member (local p : frag) b : float) : bool) : bool)\n"
             "    (then\n"
             "      (assign (local x : float) = (lit 1.0 : float))))\n"
             "  (if (and (call less_equal (lit 0.0 : float) (block $chained\n"
             "      (let:temporary chained : float = (call multiply (member (local p : frag) a : float) (lit 2.0 : "
             "float) : float))\n"
             "      (leave $chained (local chained : float)) : float) : bool) (call less (local chained : float) (lit "
             "1.0 : float) : bool) : bool)\n"
             "    (then\n"
             "      (assign (local x : float) = (lit 2.0 : float))))\n"
             "  (if (and (call less_equal (lit 0.0 : float) (block $chained_1\n"
             "      (let:temporary chained_1 : float = (local x : float))\n"
             "      (leave $chained_1 (local chained_1 : float)) : float) : bool) (call less (local chained_1 : float) "
             "(lit 1.0 : float) : bool) : bool)\n"
             "    (then\n"
             "      (assign (local x : float) = (lit 3.0 : float))))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - a call is a block named after its callee: arguments bound in order, return as leave")
{
    CHECK(structured_dump("fun grade(x: float, limit: float, scale: float) -> float:\n"
                          "    if x < limit => return 0.0\n"
                          "    return x * scale\n",
                          "let limit = 0.5\n"
                          "let x = grade(p.a * 2.0, limit, 4.0)\n")
          == "  (let limit : float = (lit 0.5 : float))\n"
             "  (let x_1 : float = (block $grade\n"
             "      (let x : float = (call multiply (member (local p : frag) a : float) (lit 2.0 : float) : float))\n"
             "      (if (call less (local x : float) (local limit : float) : bool)\n"
             "        (then\n"
             "          (leave $grade (lit 0.0 : float))))\n"
             "      (leave $grade (call multiply (local x : float) (lit 4.0 : float) : float)) : float))\n"
             "  (return (construct\n"
             "    (construct (local x_1 : float) (local x_1 : float) (local x_1 : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - a conversion the program declares is inlined like any call, and runs")
{
    constexpr auto helpers = "struct meters:\n"
                             "    value: float\n"
                             "\n"
                             "@operator(\"as\") fun to_meters(x: float) -> meters => meters(x * 4.0)\n"
                             "\n";
    constexpr auto lines = "let m = p.a as meters\n"
                           "let x = m.value\n";
    CHECK(structured_dump(helpers, lines).contains("(block $to_meters"));

    auto const checked = check_program(grey_source(helpers, lines));
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& m = checked.module;
    CHECK(sgl::check::dump(sgl::check::interpret(m, m.entry_points[0], test_inputs(m))) == "ok 1 1 1 1");
}

TEST("sgl check - a function inlined twice gets distinct names, and an arrow body is one leave")
{
    CHECK(structured_dump("fun half(x: float) -> float => x * 0.5\n"
                          "fun quarter(x: float) -> float:\n"
                          "    let h = half x\n"
                          "    return half h\n",
                          "let x = quarter(p.a + 1.0) + quarter(p.b + 1.0)\n")
          == "  (let x_2 : float = (call add (block $quarter\n"
             "      (let x : float = (call add (member (local p : frag) a : float) (lit 1.0 : float) : float))\n"
             "      (let h : float = (block $half\n"
             "          (leave $half (call multiply (local x : float) (lit 0.5 : float) : float)) : float))\n"
             "      (leave $quarter (block $half_1\n"
             "          (leave $half_1 (call multiply (local h : float) (lit 0.5 : float) : float)) : float)) : float) "
             "(block $quarter_1\n"
             "      (let x_1 : float = (call add (member (local p : frag) b : float) (lit 1.0 : float) : float))\n"
             "      (let h_1 : float = (block $half_2\n"
             "          (leave $half_2 (call multiply (local x_1 : float) (lit 0.5 : float) : float)) : float))\n"
             "      (leave $quarter_1 (block $half_3\n"
             "          (leave $half_3 (call multiply (local h_1 : float) (lit 0.5 : float) : float)) : float)) : "
             "float) : float))\n"
             "  (return (construct\n"
             "    (construct (local x_2 : float) (local x_2 : float) (local x_2 : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - a function that returns nothing is a block statement, and print is a statement of the tree")
{
    CHECK(structured_dump("fun note(x: float):\n"
                          "    if x < 0.0 => return\n"
                          "    print x\n",
                          "note p.a\nlet x = p.b\n")
          == "  (block $note\n"
             "    (let x : float = (member (local p : frag) a : float))\n"
             "    (if (call less (local x : float) (lit 0.0 : float) : bool)\n"
             "      (then\n"
             "        (leave $note)))\n"
             "    (print (local x : float)))\n"
             "  (let x_1 : float = (member (local p : frag) b : float))\n"
             "  (return (construct\n"
             "    (construct (local x_1 : float) (local x_1 : float) (local x_1 : float) (lit 1.0 : float) : float4) : "
             "target))\n");
}

TEST("sgl check - a flat node keeps its origin and the chain of call sites it was inlined through")
{
    auto const checked = check_program(cc::string("fun half(x: float) -> float => x * 0.5\n"
                                                  "fun quarter(x: float) -> float:\n"
                                                  "    let h = half x\n"
                                                  "    return half h\n")
                                       + grey_source("", "let x = quarter p.a\n"));
    REQUIRE(reports_of(checked) == "");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& e = checked.module.entry_points[0];

    auto const text_of_call = [&](sgl::check::call_site const& site)
    { return cc::string(checked.user.text_of(checked.user.at(checked.user_ast.at(site.call).form).where)); };

    // every `x * 0.5` of `half`, by the chain it came through
    auto chains = cc::string();
    for (auto const& x : e.exprs)
    {
        auto const* const call = x.node.try_as<sgl::check::flat_call>();
        if (call == nullptr || checked.module.at(call->callee).name != "multiply")
            continue;
        CHECK(checked.user.text_of(checked.user.at(checked.user_ast.at(x.from.expr).form).where) == "x * 0.5");
        for (auto const& site : e.at(x.inlined_through))
            chains += "[" + text_of_call(site) + "]";
        chains += "\n";
    }
    CHECK(chains == "[quarter p.a][half x]\n[quarter p.a][half h]\n");

    // a node of the entry point's own body came through nothing
    for (auto const& s : e.at(e.body))
        CHECK(e.at(s).inlined_through.empty());
    // and the `let h` of `quarter` came through the one call of it
    auto lets = 0;
    for (auto const& s : e.stmts)
        if (auto const* const let = s.node.try_as<sgl::check::flat_let>(); let != nullptr && e.at(let->local).name == "h")
        {
            ++lets;
            REQUIRE(e.at(s.inlined_through).size() == 1);
            CHECK(text_of_call(e.at(s.inlined_through)[0]) == "quarter p.a");
        }
    CHECK(lets == 1);
}
