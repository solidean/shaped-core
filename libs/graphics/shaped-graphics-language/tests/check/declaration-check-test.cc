#include "check-test-support.hh"

using namespace sgl_test;
using sgl::check::symbol_state;

TEST("sgl check - a @builtin is keyed by its name, and an unknown name is unknown-builtin")
{
    CHECK(reports_for("@builtin struct quaternion\n") == "unknown-builtin user:[quaternion] quaternion\n");
    CHECK(reports_for("@builtin fun frobnicate(x: float) -> float\n") == "unknown-builtin user:[frobnicate] frobnicate\n");

    // The name of a builtin function declares no builtin struct, and the other way round.
    auto const swapped = check_sources("", "@builtin struct dot\n@builtin fun ray_query(v: dot) -> dot\n");
    CHECK(reports_of(swapped) == "unknown-builtin user:[dot] dot\nunknown-builtin user:[ray_query] ray_query\n");

    // A builtin function is one OVERLOAD: its name together with its parameter types, as the registry holds them.
    CHECK(reports_for("@builtin fun dot(a: pos3, b: pos3) -> float\n") == "unknown-builtin user:[dot] dot(pos3, pos3)\n");
    CHECK(reports_for("struct mine:\n    x: float\n@builtin fun length(v: mine) -> float\n")
          == "unknown-builtin user:[length] length(mine)\n");
}

TEST("sgl check - an opaque struct needs @builtin")
{
    CHECK(reports_for("struct handle\n") == "opaque-struct-needs-builtin user:[handle] handle\n");
    CHECK(reports_for("struct pair:\n    a: float\n    b: float\n") == "");

    // A diagnostic names its file by position, since a span alone does not: the prelude is 0 and the user file 1.
    CHECK(sgl::check::dump_diagnostics(check_sources("", "struct handle\n").module)
          == "opaque-struct-needs-builtin @1:7+6 handle\n");
    CHECK(sgl::check::dump_diagnostics(check_sources("struct handle\n", "").module)
          == "opaque-struct-needs-builtin @0:7+6 handle\n");
}

TEST("sgl check - a name is declared once, unless it is functions and at most one struct in front of them")
{
    CHECK(reports_for("struct a:\n    x: float\nstruct a:\n    y: float\n") == "duplicate-declaration user:[a] a\n");
    CHECK(reports_for("struct a:\n    x: float\nenum a:\n    y\n") == "duplicate-declaration user:[a] a\n");
    CHECK(reports_for("fun a(k: float) -> float => k\nbinding a:\n    y: float\n")
          == "duplicate-declaration user:[a] a\n");
    // CHK-240: functions of a struct's name overload its constructor, whichever of them stands first
    CHECK(reports_for("struct a:\n    x: float\nfun a(k: float, l: float) -> a => a(k + l)\n") == "");
    CHECK(reports_for("fun a(k: float, l: float) -> a => a(k + l)\nstruct a:\n    x: float\n") == "");
    CHECK(reports_for("struct a:\n    x: float\n    x: float\n") == "duplicate-declaration user:[x] x\n");
    CHECK(reports_for("@builtin fun dot(a: float3, a: float3) -> float\n") == "duplicate-declaration user:[a] a\n");

    // two functions of one name are an overload set
    CHECK(reports_for("@builtin fun dot(a: float3, b: float3) -> float\n") == "");
}

TEST("sgl check - the prelude is a scope around the user file's, which may shadow its names")
{
    // the user file's struct is the one its lookups find: the prelude's vec3 has no member `weight`
    // A builtin type is @shadowable(false), so this and the int below need a prelude whose types may be hidden.
    auto const shadowable = shadowable_builtins_text();
    CHECK(reports_of(check_sources(shadowable, "struct vec3:\n    weight: float\nfun g(v: vec3) -> float:\n    return "
                                               "v.weight\n"))
          == "");
    // with the library's prelude it is reported once, although the struct and its constructor are two symbols
    CHECK(reports_for("struct vec3:\n    weight: float\n")
          == "shadows-unshadowable user:[struct vec3:] vec3 is @shadowable(false) in the prelude\n");
    CHECK(reports_for("binding dot:\n    x: float\n") == "");
    // a function hides a struct of the prelude, and a struct the prelude's functions, unless both are functions
    CHECK(reports_for("fun vec3(k: float) -> float => k\nfun g(k: float) -> float:\n    return vec3(k)\n") == "");
    CHECK(reports_for("struct dot:\n    a: float\nfun g(d: dot) -> float:\n    return d.a\n") == "");
    // what the checker needs of the prelude by name stays the prelude's: a for runs over its int
    CHECK(reports_of(check_sources(shadowable, "struct int:\n    a: float\nfun g(k: float) -> float:\n    for i in 0 "
                                               "..< 3:\n"
                                               "        return k\n    return k\n"))
          == "");

    // a prelude file never sees the user file's names
    cc::string_view const hidden[] = {"@builtin struct float\n", "struct u:\n    y: t\n", "struct t:\n    x: u\n"};
    CHECK(reports_of(check_files(hidden)) == "unknown-name prelude.1:[t] t\n");
    // the files of the prelude share one scope, as the user file has one
    cc::string_view const twice[] = {"@builtin struct float\n", "struct a:\n    x: float\n",
                                     "struct a:\n    y: float\n", "struct a:\n    z: float\n"};
    CHECK(reports_of(check_files(twice)) == "duplicate-declaration prelude.2:[a] a\n");
}

TEST("sgl check - a symbol that needs itself is a dependency cycle that names the loop")
{
    // A struct field is the smallest construct of the tracer that can form a cycle: no constant, alias or inferred
    // return type is checked yet, and a function body demands nothing.
    auto const two = check_sources(read_prelude(), "struct a:\n    x: b\nstruct b:\n    y: a\n");
    CHECK(reports_of(two) == "dependency-cycle user:[a] a -> b -> a\n");
    // The cycle costs one field its type, and every symbol still settles.
    auto const dump = sgl::check::dump(two.module);
    CHECK(dump.contains("(struct a (x : b))\n"));
    CHECK(dump.contains("(struct b (y : <error>))\n"));
    for (auto const& s : two.module.symbols)
        CHECK(s.state == symbol_state::checked);

    CHECK(reports_for("struct s:\n    next: s\n") == "dependency-cycle user:[s] s -> s\n");
    CHECK(reports_for("struct a:\n    x: b\nstruct b:\n    x: c\nstruct c:\n    x: a\n")
          == "dependency-cycle user:[a] a -> b -> c -> a\n");
    // the loop leaves out what merely led into it
    CHECK(reports_for("struct outer:\n    x: a\nstruct a:\n    x: b\nstruct b:\n    y: a\n")
          == "dependency-cycle user:[a] a -> b -> a\n");
}

TEST("sgl check - compilation is on demand, so a declaration may stand below its use")
{
    auto const checked
        = check_sources(read_prelude(), "struct outer:\n    inner: inner\nstruct inner:\n    x: float\n");
    CHECK(reports_of(checked) == "");
    // `inner` was compiled from inside `outer`, so its type exists first
    auto const& m = checked.module;
    auto const outer = symbol_named(m, "outer");
    auto const inner = symbol_named(m, "inner");
    CHECK(sgl::check::index_of(inner.type) < sgl::check::index_of(outer.type));
}

TEST("sgl check - names in type positions")
{
    CHECK(reports_for("struct a:\n    x: nope\n") == "unknown-name user:[nope] nope\n");
    CHECK(reports_for("struct a:\n    x: normalize\n")
          == "wrong-kind-of-name user:[normalize] normalize is a function, and a type stands here\n");
    CHECK(reports_for("@builtin fun saturate(x) -> float\n") == "missing-type user:[x] x\n");
}

TEST("sgl check - a type is canonical: one id per struct, and the error type is types[0]")
{
    auto const checked = check_sources(read_prelude(), "struct a:\n    x: float\n    y: float\n");
    auto const& m = checked.module;
    CHECK(m.types[0].kind == sgl::check::type_kind::error);
    auto const& a = m.at(symbol_named(m, "a").type);
    auto const fields = m.at(a.members);
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].type == fields[1].type);
    CHECK(m.name_of(fields[0].type) == "float");
}

TEST("sgl check - a const is a literal, an enum case or another const, and stands for its value")
{
    // CHK-219
    CHECK(reports_for("const k = 1.5\nconst n = -3\nconst m = n\nenum e:\n    a\n    b\nconst first = e.a\n"
                      "fun f(x: float) -> float:\n    if n < m or first == e.b => return x * k\n    return x\n")
          == "");
    CHECK(reports_for("const k: int = 1.5\n") == "type-mismatch user:[1.5] expected int, got float\n");
    // a uint const is written with its suffix, and is neither negative nor wider than 32 bits
    CHECK(reports_for("const mask: uint = 0xff00u\ntest (0xffffu & mask) == mask\n") == "");
    CHECK(reports_for("const k = -1u\ntest k == 0u\n").contains("type-mismatch user:[-1u] -1u is no uint"));
    CHECK(reports_for("const k = 4294967296u\ntest k == 0u\n").contains("4294967296u is no uint"));
    CHECK(reports_for("const n = 4u\nfun f() -> float[n] => float[n].filled(0.0)\n").contains("int"));
    CHECK(reports_for("const a = b\nconst b = a\n").contains("dependency-cycle"));
    CHECK(reports_for("const k = 1.0\nstruct a:\n    x: k\n")
          == "wrong-kind-of-name user:[k] k is a const, and a type stands here\n");
    // true and false are consts of the prelude, and a case over a bool that names both is exhaustive (CHK-221)
    CHECK(reports_for("fun f(b: bool) -> float:\n    if b == true => return 1.0\n    return case b:\n"
                      "        true => 0.5\n        false => 0.0\n")
          == "");
}

TEST("sgl check - a computed const reads no binding, and one without a value says why")
{
    // CHK-219: its value is checked in a scope that lists no binding, as a test's is
    constexpr auto cells = "binding cells:\n    scale: float\n\nfun scaled(){cells} -> float => cells.scale\n";
    CHECK(reports_for(cc::format("{}const k = scaled()\n", cells))
              .contains("binding-not-listed user:[scaled()] scaled needs cells, which is not in the binding list of "
                        "const k"));
    // CHK-311: a run that has no value is the const's error, at its value
    CHECK(reports_for("const z = 0\nconst k = 7 / z\n")
              .contains("constant-without-value user:[7 / z] k has no value: an integer divided by zero"));
    CHECK(reports_for("fun spin() -> int:\n    let mut i = 0\n    while i >= 0:\n        i = (i + 1) % 7\n    "
                      "return i\nconst k = spin()\n")
              .contains("constant-without-value user:[spin()] k takes more than 100000 steps to compute"));
    // CHK-353: an option is a bool, a scalar or an enum case, which a computed value of another type is not
    CHECK(reports_for("@option const v = float2(1.0, 2.0)\n").contains("an option of this type"));
    // a computed int is the same constant a literal is, so an array length and a workgroup size take it
    CHECK(reports_for("const n = 2 * 4\nfun f() -> float[n] => float[n].filled(0.0)\n") == "");
}

TEST("sgl check - a @shadowable(false) symbol is hidden by nothing")
{
    // CHK-220: not by a declaration of the program, a local, a parameter or a for variable
    CHECK(reports_for("const true = 1\n").contains("shadows-unshadowable"));
    CHECK(reports_for("fun f(k: float) -> float:\n    let true = k\n    return k\n")
          == "shadows-unshadowable user:[true] true is @shadowable(false)\n");
    CHECK(reports_for("fun f(false: float) -> float => false\n").contains("shadows-unshadowable user:[false]"));
    CHECK(reports_for("fun f(k: float) -> float:\n    for true in 0 ..< 3:\n        return k\n    return k\n")
              .contains("shadows-unshadowable user:[true]"));
    // a symbol of the program may say so of itself
    CHECK(reports_for("@shadowable(false) const limit = 4\nfun f(limit: int) -> int => limit\n")
          == "shadows-unshadowable user:[limit] limit is @shadowable(false)\n");
    CHECK(reports_for("@shadowable(maybe) const limit = 4\n")
          == "invalid-attribute-arguments user:[shadowable] @shadowable takes `false` or `true`, as in "
             "@shadowable(false)\n");
    // and on every declaration it stands on, where a typo would otherwise leave the symbol hidable
    cc::string_view const declarations[] = {
        "@shadowable(flase) struct light:\n    power: float\n", "@shadowable(flase) enum mode:\n    on\n    off\n",
        "@shadowable(flase) fun f() -> float => 1.0\n",         "@shadowable(flase) binding frame:\n    e: float\n",
        "@shadowable struct light:\n    power: float\n",
    };
    for (auto const d : declarations)
        CHECK(reports_for(d).starts_with("invalid-attribute-arguments user:[shadowable]")).dump("source", d);
    CHECK(reports_for("@shadowable(true) struct light:\n    power: float\n") == "");
}

TEST("sgl check - what the tracer does not carry is unsupported-yet, and names the construct")
{
    CHECK(reports_for("struct a:\n    x: float\n    fun reset(mut self):\n        self.x = 0.0\n")
          == "unsupported-yet user:[reset] mut self\n");
    CHECK(reports_for("binding b = constants\n") == "unsupported-yet user:[constants] a binding composition\n");
    // CHK-25: a format is no type, so a texture takes what it samples to and an image takes a format as a case.
    CHECK(reports_for("binding b:\n    t: texture_2d[rgba8]\n") == "unknown-name user:[rgba8] rgba8\n");
    CHECK(reports_for("@format(rgba8) struct a:\n    x: float\n")
          == "unsupported-yet user:[format] the attribute @format on a struct\n");

    // an alias whose value the tracer does not carry names no type, and a use of it is silent
    CHECK(reports_for("type pair = (float, float)\nstruct a:\n    x: pair\n")
          == "unsupported-yet user:[(float, float)] a tuple type\n");
    // the module line is accepted
    CHECK(reports_for("module cube\nstruct a:\n    x: float\n") == "");
}

TEST("sgl check - two aliases that name each other are a dependency cycle, and name no type")
{
    CHECK(reports_for("type ping = pong\ntype pong = ping\nstruct a:\n    x: ping\n")
          == "dependency-cycle user:[ping] ping -> pong -> ping\n");
    CHECK(reports_for("type again = again\n") == "dependency-cycle user:[again] again -> again\n");
}

TEST("sgl check - a function needs a body unless it is @builtin")
{
    CHECK(reports_for("fun f(x: float) -> float\n") == "expected-body user:[f] f\n");
}

TEST("sgl check - a generic body is checked per instantiation, and its names where it is written")
{
    // CHK-338: a body may do with a value of a type parameter whatever each instantiation's arguments allow
    CHECK(reports_for("fun twice[T](x: T) -> T => x + x\nfun f() -> int => twice(2)\nfun g() -> float => twice(1.5)\n")
          == "");
    CHECK(reports_for("fun twice[T](x: T) => x + x\nfun f() -> int => twice(2)\n") == "");
    // nothing instantiates it, so only its names are checked, and a warning says so
    CHECK(reports_for("fun twice[T](x: T) -> T => x + x\n")
          == "generic-not-instantiated user:[twice] nothing instantiates twice, so only its names are checked\n");
    CHECK(reports_for("fun twice[T](x: T) -> T => x + nope\nfun f() -> int => twice(2)\n")
              .starts_with("unknown-name user:[nope]"));
    // an error is reported where the body says it, under the instantiation that reached it
    auto const negated = reports_for("fun negated[T](x: T) -> T => not x\nfun f() -> int => negated(2)\n");
    CHECK(negated.starts_with("no-matching-overload user:[not x]"));
    CHECK(negated.contains("  note user:[negated(2)] in negated[int], instantiated here\n"));
    // the same error from two instantiations is reported once
    auto const twice_wrong
        = reports_for("fun wrong[T](x: T) -> T:\n    let k: int = 1.5\n    return x\nfun f() -> int => wrong(1)\n"
                      "fun g() -> float => wrong(1.5)\n");
    CHECK(twice_wrong.starts_with("literal-not-representable user:[1.5]"));
    CHECK(!cc::string_view(twice_wrong).subview(1).contains("literal-not-representable"));
    // CHK-340: a type parameter the arguments leave open is said by where the call stands, or the call is an error.
    // Only the prelude has a value of an open type to give, `undefined()`, so `pick` stands in a prelude file.
    auto const builtins = builtins_text();
    cc::string_view const open_result[]
        = {builtins, "fun pass[T](x: T) -> T => x\nfun pick[T](x: float) -> T => pass(undefined())\n",
           "fun f() -> float => pick(1.0)\nfun g():\n    let y = pick(1.0)\n"};
    CHECK(reports_of(check_files(open_result))
          == "no-matching-overload user:[pick(1.0)] T of pick is said by nothing: neither an argument nor where the "
             "call stands\n");
    // an entry point is handed values of known types
    CHECK(reports_for("@compute(1) fun main[T](@thread_id id: int3):\n    return\n")
          == "wrong-kind-of-name user:[main] an entry point is no generic function: the GPU hands it values of known "
             "types\n");
    // CHK-338: a type parameter hides every symbol of its name, and names no value
    auto const not_instantiated
        = cc::string("generic-not-instantiated user:[f] nothing instantiates f, so only its names are checked\n");
    // a call of one is a call of what it stands for in each instance, here `float(1.0).x`
    CHECK(reports_for("struct A:\n    x: float\nfun f[A](v: A) -> float => A(1.0).x\n") == not_instantiated);
    CHECK(reports_for("struct A:\n    x: float\nfun f[A](v: A) -> float => A(1.0).x\nfun g() -> float => f(2.0)\n")
              .contains("note user:[f(2.0)] in f[float], instantiated here"));
    CHECK(reports_for("fun f[A](v: A) -> A:\n    let k = A\n    return v\n")
          == "wrong-kind-of-name user:[A] A is a type parameter, which names a type and no value\n" + not_instantiated);
    // `n: float` is a value parameter, which names a value and no type; `T: type` is a type parameter spelled out
    CHECK(reports_for("fun f[T: float](x: T) => x\n")
              .starts_with("wrong-kind-of-name user:[T] T is a value parameter, which names a value and no type\n"));
    CHECK(reports_for("fun f[T: type](x: T) -> T => x\nfun g() -> int => f(1)\n") == "");
    CHECK(reports_for("fun f[n: float3](x: float) => x\n")
              .starts_with("unsupported-yet user:[float3] a value parameter of float3, where int, uint, bool, float "
                           "and "
                           "enums are"));
    // a default is of the parameter's kind; an attribute on a parameter waits
    CHECK(reports_for("fun f[T = float](x: int) -> T => T(x)\nfun g() -> float => f(1)\n") == "");
    CHECK(reports_for("fun f[n: int = float](x: float) => x\n")
              .starts_with("invalid-constant-argument user:[float] n is a value parameter of int"));
    CHECK(reports_for("fun f[@x T](x: T) => x\n")
              .starts_with("unsupported-yet user:[T] a type parameter with an attribute\n"));
}

TEST("sgl check - an instance checks only the const if branch its parameters leave")
{
    // CHK-430: the instance for float never checks the shift, which no float has
    CHECK(reports_for("fun h[T](x: T) -> T:\n    const if T == float:\n        return x * 0.5\n    else:\n"
                      "        return x >> 1\nfun f() -> float => h(2.0)\nfun g() -> int => h(2)\n")
          == "");
    // a plain `if` decides nothing before inlining, so each instance checks both branches
    CHECK(reports_for("fun h[T](x: T) -> T:\n    if T == float:\n        return x * 0.5\n    else:\n"
                      "        return x >> 1\nfun f() -> float => h(2.0)\n")
              .starts_with("no-matching-overload user:[x >> 1]"));
    // a condition reading the target is known by no instance
    CHECK(reports_for("fun h[T](x: T) -> T:\n    const if target_api == .dx12 and T == float:\n        return x >> 1\n"
                      "    else:\n        return x\nfun f() -> float => h(2.0)\n")
              .starts_with("no-matching-overload user:[x >> 1]"));
    // CHK-429: a type compares with a type alone
    CHECK(reports_for("fun h[T](x: T) -> bool => T == x\nfun f() -> bool => h(1)\n")
              .starts_with("type-mismatch user:[x] a type compares with a type"));
    // what follows a chain an instance decided serves the instances that take another branch
    CHECK(reports_for("fun h[T](x: T) -> int:\n    const if T == int:\n        return x\n    return 0\n"
                      "fun f() -> int => h(3)\nfun g() -> int => h(3.0)\n")
          == "");
}

TEST("sgl check - a generic struct takes one argument per parameter, each of its kind")
{
    // CHK-339
    CHECK(reports_for("struct box[A]:\n    v: A\n") == "");
    CHECK(reports_for("fun f(r: report[float, float]) => 1.0\n")
          == "wrong-kind-of-name user:[report[float, float]] report takes 1 argument\n");
    // a value parameter takes an int constant, and a type parameter a type
    CHECK(reports_for("struct ring[n: int, T]:\n    items: T[n]\nfun f(r: ring[float, float]) => 1.0\n")
              .starts_with("invalid-constant-argument user:[float] n is a value parameter of int"));
    CHECK(reports_for("struct ring[n: int, T]:\n    items: T[n]\nfun f(r: ring[2, 3]) => 1.0\n")
              .starts_with("wrong-kind-of-name user:[3]"));
    // an array's length names an int value parameter, and nothing else of the parameters
    CHECK(reports_for("struct bad[n: float]:\n    items: float[n]\n")
              .starts_with("invalid-constant-argument user:[n] an array's length is an int constant of at least 1, or "
                           "an "
                           "int value parameter"));
}

TEST("sgl check - a mut parameter is spelled on its type, takes no default, and no entry point has one")
{
    // CHK-315
    CHECK(reports_for("fun f(mut p: float) => p\n") == "unexpected-keyword user:[p] `mut p: T` is spelled `p: mut T`\n");
    CHECK(reports_for("@compute(1) fun main(@thread_id id: int3, x: mut float):\n    return\n")
          == "wrong-kind-of-name user:[x] x is a parameter of an entry point, which the GPU fills and no caller hands "
             "a "
             "place\n");
    // CHK-316: only the caller's place fills it
    CHECK(reports_for("fun bump(p: mut int = 0):\n    p += 1\n")
          == "default-not-allowed-here user:[0] p is a mut parameter, which only a caller's place fills\n");
}

TEST("sgl check - a function type is a parameter's whole type, and nothing else's")
{
    // CHK-317
    auto const refused = cc::string("a function type is the type of a parameter, and of nothing else\n");
    CHECK(reports_for("fun g(f: mut (float) -> float) => 1.0\n")
          == "wrong-kind-of-name user:[(float) -> float] " + refused);
    CHECK(reports_for("fun g(fs: ((float) -> float)[2]) => 1.0\n")
          == "wrong-kind-of-name user:[(float) -> float] " + refused);
    CHECK(reports_for("fun g(r: report[(float) -> float]) => 1.0\n")
          == "wrong-kind-of-name user:[(float) -> float] " + refused);
    CHECK(reports_for("struct s:\n    f: (float) -> float\n") == "wrong-kind-of-name user:[(float) -> float] " + refused);
    CHECK(reports_for("fun g(x: float) -> (float) -> float => g\n")
              .starts_with("wrong-kind-of-name user:[(float) -> float] " + refused));
}

TEST("sgl check - @bitflags is the prelude's")
{
    // CHK-321
    CHECK(reports_for("@bitflags enum e:\n    a = 1\n")
          == "unsupported-yet user:[bitflags] @bitflags on an enum of the program\n");
}

TEST("sgl check - @operator hides the function's name, and takes one quoted operator")
{
    auto const source = cc::string_view("fun f(a: float) -> float:\n    return multiply(a, a)\n");
    CHECK(reports_for(source) == "unknown-name user:[multiply] multiply\n");

    CHECK(reports_for("@builtin @operator(1.0) fun multiply(a: float3, b: float3) -> float3\n")
          == "invalid-attribute-arguments user:[operator] @operator takes one quoted operator, as in "
             "@operator(\"*\")\n");
    CHECK(reports_for("@builtin(1.0) struct mat4x\n")
              .contains("invalid-attribute-arguments user:[(1.0)] @builtin takes no arguments\n"));
}

TEST("sgl check - a binding records @inline and its members")
{
    auto const checked
        = check_sources(read_prelude(), "@inline binding a:\n    m: mat4\nbinding b:\n    v: vec3\n    k: float\n");
    CHECK(reports_of(checked) == "");
    CHECK(
        sgl::check::dump(checked.module).ends_with("(binding a inline (m : mat4))\n(binding b (v : vec3) (k : float))\n"));
    // the prelude's reserved bindings stand ahead of the program's
    auto const& all = checked.module.bindings;
    REQUIRE(all.size() >= 2);
    CHECK(all[all.size() - 2].is_inline);
    CHECK(!all[all.size() - 1].is_inline);
}

TEST("sgl check - the binding list is checked: every entry is the name of a binding")
{
    CHECK(reports_for("fun f(x: float){nope} -> float => x\n") == "unknown-name user:[nope] nope\n");
    CHECK(reports_for("fun f(x: float){vec3} -> float => x\n") == "wrong-kind-of-name user:[vec3] vec3 is no binding\n");
    CHECK(reports_for("binding b:\n    k: float\nfun f(x: float){b = b} -> float => x\n")
          == "unsupported-yet user:[b = b] a binding entry that is not a binding's name\n");
}

namespace
{
constexpr auto edges = cc::string_view("@vertex struct vin:\n    p: pos3\n"
                                       "struct vout:\n    @position p: hpos4\n"
                                       "@pixel struct target:\n    c: float4\n"
                                       "struct plain:\n    p: pos3\n"
                                       "struct two:\n    @position a: hpos4\n    @position b: hpos4\n"
                                       "struct wrong:\n    @position a: float4\n");

cc::string entry_reports(cc::string_view functions)
{
    return reports_for(cc::string(edges) + functions);
}
} // namespace

TEST("sgl check - an entry point's signature follows the rules of its stage")
{
    CHECK(entry_reports("@vertex fun vs(v: vin) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n") == "");

    CHECK(entry_reports("@vertex fun vs(v: plain) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] the struct parameter of a @vertex fun is a @vertex struct\n");
    CHECK(entry_reports("@vertex fun vs(v: vin, w: vin) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] an entry point takes its stage struct first, and stage inputs after it\n");
    // CHK-271: a vertex stage may draw from no vertex buffer, and takes its stage inputs after its struct
    CHECK(entry_reports("@vertex fun vs(@vertex_index i: int) -> vout:\n    return { p = hpos4(0.0, 0.0, 0.0, 1.0) }\n")
          == "");
    CHECK(entry_reports("@vertex fun vs(v: vin, @instance_index i: int, @vertex_index k: int) -> vout:\n"
                        "    return { p = hpos4(..v.p, 1.0) }\n")
          == "");
    CHECK(entry_reports("@vertex fun vs(@vertex_index i: int, v: vin) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] an entry point takes its stage struct first, and stage inputs after it\n");
    CHECK(entry_reports("@vertex fun vs(v: vin, @vertex_index i: uint) -> vout:\n    return { p = hpos4(..v.p, 1.0) "
                        "}\n")
          == "invalid-entry-point user:[vs] a @vertex_index parameter is an int\n");
    // a vertex that a tessellation stage reads first has no @position; the pipeline asks for it where the rasterizer reads it
    CHECK(entry_reports("@vertex fun vs(v: vin) -> plain:\n    return { p = v.p }\n") == "");
    CHECK(entry_reports("@vertex fun vs(v: vin) -> two:\n    return { a = hpos4(..v.p, 1.0), b = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] a @vertex fun returns a struct with at most one @position field\n");
    CHECK(entry_reports("@vertex fun vs(v: vin) -> wrong:\n    return { a = float4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] the @position field of a @vertex fun is an hpos4\n");

    CHECK(entry_reports("@pixel fun ps(v: vout) -> target:\n    return { c = float4(..v.p) }\n") == "");
    CHECK(entry_reports("@pixel fun ps(v: vout) -> plain:\n    return { p = pos3(1.0, 1.0, 1.0) }\n")
          == "invalid-entry-point user:[ps] a @pixel fun returns a @pixel struct, one color value or nothing\n");
    // CHK-418: one color value is a target of its own, and nothing is no target; a strong vector or a bool is neither
    CHECK(entry_reports("@pixel fun ps(v: vout) -> float4 => float4(..v.p)\n") == "");
    CHECK(entry_reports("@pixel fun ps(v: vout) -> uint2 => uint2(1u, 2u)\n") == "");
    CHECK(entry_reports("@pixel fun ps(v: vout):\n    let unused = v.p\n") == "");
    CHECK(entry_reports("@pixel fun ps(v: vout) -> bool => true\n")
          == "invalid-entry-point user:[ps] a @pixel fun returns a @pixel struct, one color value or nothing\n");
    CHECK(entry_reports("@pixel fun ps(v: vout) -> vec3 => vec3(1.0, 0.0, 0.0)\n")
          == "invalid-entry-point user:[ps] a @pixel fun returns a @pixel struct, one color value or nothing\n");
    // the attributes of its result are its target's settings, and no other function's result takes one
    CHECK(entry_reports("fun f() -> @blend(.none) float4 => float4(1.0)\n").contains("an attribute on a type"));
    CHECK(entry_reports("@pixel fun ps(v: float) -> target:\n    return { c = float4(v, v, v, v) }\n")
          == "invalid-entry-point user:[ps] the struct parameter of an entry point is a struct with fields\n");
    CHECK(entry_reports("@pixel fun ps(v: vout, @is_front_facing f: bool, @sample_mask m: uint) -> target:\n"
                        "    return { c = float4(..v.p) }\n")
          == "");
    CHECK(entry_reports("@pixel fun ps(@is_front_facing f: bool) -> target:\n    return { c = float4(1.0, 1.0, 1.0, "
                        "1.0) }\n")
          == "invalid-entry-point user:[ps] a @pixel fun takes the struct its vertex stage returns\n");
    CHECK(entry_reports("@vertex @pixel fun ps(v: vout) -> target:\n    return { c = float4(..v.p) }\n")
          == "invalid-entry-point user:[ps] an entry point has one stage\n");
}

TEST("sgl check - an entry point with an error has diagnostics and no flat tree")
{
    auto const good = check_sources(
        read_prelude(), cc::string(edges) + "@pixel fun ps(v: vout) -> target:\n    return { c = float4(..v.p) }\n");
    CHECK(good.module.entry_points.size() == 1);

    auto const bad = check_sources(read_prelude(),
                                   cc::string(edges) + "@pixel fun ps(v: vout) -> target:\n    return { c = nope }\n");
    CHECK(reports_of(bad) == "unknown-name user:[nope] nope\n");
    CHECK(bad.module.entry_points.empty());

    auto const invalid = check_sources(
        read_prelude(), cc::string(edges) + "@pixel fun ps(v: vout) -> plain:\n    return { p = pos3(1.0, 1.0, 1.0) }\n");
    CHECK(invalid.module.entry_points.empty());
}

TEST("sgl check - a default is checked once, where it is declared, and reads only the parameters before it")
{
    CHECK(reports_for("fun f(x: float, y: float = true) -> float => x + y\n")
          == "type-mismatch user:[true] the default of y is float, got bool\n");
    CHECK(reports_for("fun f(x: float = y, y: float = 1.0) -> float => x + y\n") == "unknown-name user:[y] y\n");
    // a field's default is its constructor parameter's, and reads the fields before it
    CHECK(reports_for("struct s:\n    a: float = b\n    b: float = 1.0\n") == "unknown-name user:[b] b\n");
    CHECK(reports_for("struct s:\n    a: float\n    b: float = a * 2.0\n") == "");
}

TEST("sgl check - @swizzle takes two to four one-letter fields of one type with vectors, and owns their names")
{
    CHECK(reports_for("@swizzle struct rgb:\n    r: float\n    g: float\n    b: float\n") == "");
    CHECK(reports_for("@swizzle struct mask:\n    a: bool\n    b: bool\n") == "");
    CHECK(reports_for("@swizzle struct ab:\n    a: float\n    bb: float\n")
          == "invalid-attribute-arguments user:[swizzle] bb is no one-letter field, and a swizzle names its fields by "
             "their letters\n");
    CHECK(reports_for("@swizzle struct ab:\n    a: float\n    b: int\n")
          == "invalid-attribute-arguments user:[swizzle] b is int and a is float, and a swizzle's fields are of one "
             "type\n");
    CHECK(reports_for("@swizzle struct one:\n    a: float\n")
          == "invalid-attribute-arguments user:[swizzle] a swizzle reads two to four fields, and one has 1\n");
    CHECK(reports_for("@swizzle struct ab:\n    a: mat4\n    b: mat4\n")
          == "invalid-attribute-arguments user:[swizzle] a is mat4, which has no vectors\n");

    // CHK-386: a function or a property named like a swizzle would never be reached, an extension's included
    CHECK(reports_for("fun float3.zy(self) -> float => 1.0\n")
          == "member-name-clash user:[zy] zy is a swizzle of float3 already\n");
    CHECK(reports_for("fun float3.xxx => 1.0\n") == "member-name-clash user:[xxx] xxx is a swizzle of float3 already\n");
    CHECK(reports_for("@swizzle struct rgb:\n    r: float\n    g: float\n    b: float\n    gr => 1.0\n")
          == "member-name-clash user:[gr] gr is a swizzle of rgb already\n");
    // a name with a letter that is no field is free, and so is one on a struct without the attribute
    CHECK(reports_for("fun float3.xyzw => 1.0\n") == "");
    CHECK(reports_for("struct rgb:\n    r: float\n    g: float\n    gr => 1.0\n") == "");
}

TEST("sgl check - a type scope holds one kind of thing per name, and an extension names a type")
{
    CHECK(reports_for("struct s:\n    x: float\n    fun x(self) -> float => 1.0\n")
          == "member-name-clash user:[x] x is a field of s already\n");
    CHECK(reports_for("struct s:\n    x: float\n    y => self.x\n    fun y(self, k: float) -> float => k\n")
          == "member-name-clash user:[y] y is a property of s already\n");
    CHECK(reports_for("enum e:\n    a\n    a => 1\n") == "member-name-clash user:[a] a is a case of e already\n");
    // an extension is checked against the type's own members the same way
    CHECK(reports_for("struct s:\n    x: float\nfun s.x => 1.0\n")
          == "member-name-clash user:[x] x is a field of s already\n");
    // methods of one name are an overload set, in the type and through extensions alike
    CHECK(reports_for("struct s:\n    x: float\n    fun f(self) -> float => self.x\nfun s.f(self, k: float) -> float "
                      "=> k\n")
          == "");
    // CHK-62: a member body reads its receiver's members through `self` alone
    CHECK(reports_for("struct s:\n    x: float\n    fun f(self) -> float => x\n") == "unknown-name user:[x] x\n");
    CHECK(reports_for("struct s:\n    x: float\n    y => x\n") == "unknown-name user:[x] x\n");

    // CHK-237: the extended type is a struct or an enum
    CHECK(reports_for("fun nothing.f(self) -> float => 1.0\n") == "unknown-name user:[nothing] nothing\n");
    CHECK(reports_for("fun dot.f(self) -> float => 1.0\n")
          == "wrong-kind-of-name user:[dot] dot is no struct and no enum, and only a type has functions of its own\n");
    // `self` belongs to a function of a type scope
    CHECK(reports_for("fun f(self) -> float => 1.0\n")
          == "wrong-kind-of-name user:[f] self is the receiver of a method, and this function belongs to no type\n");
    CHECK(reports_for("fun f() -> float => self\n") == "unknown-name user:[self] self\n");
}

TEST("sgl check - a member crossing stages says how it is interpolated, and an integer crosses only flat")
{
    // CHK-273
    constexpr auto vs = "@vertex fun vs(v: vin) -> link:\n    return { p = hpos4(..v.p, 1.0), id = 1 }\n";
    auto const with = [](cc::string_view link_member)
    {
        return reports_for(cc::string("@vertex struct vin:\n    p: pos3\nstruct link:\n    @position p: hpos4\n    ")
                           + link_member + "\n" + vs);
    };
    CHECK(with("@interpolate(.flat) id: int") == "");
    CHECK(with("id: int")
          == "invalid-entry-point user:[vs] the int member 'id' crosses stages only flat: write "
             "`@interpolate(.flat)`\n");
    CHECK(with("@interpolate(.linear) id: int").contains("crosses stages only flat"));
    // flat takes one vertex's value, so it has no sampling point
    CHECK(with("@interpolate(.flat, .centroid) id: int").contains("invalid-attribute-arguments"));
    CHECK(with("@interpolate(.sideways) id: int").contains("invalid-attribute-arguments"));

    // nothing that crosses no stage edge is interpolated
    CHECK(reports_for("@vertex struct vin:\n    @interpolate(.flat) p: pos3\nstruct vout:\n    @position p: hpos4\n"
                      "@vertex fun vs(v: vin) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n")
              .contains("crosses no stage edge"));
}

TEST("sgl check - a pixel stage interpolating per sample needs sample_rate_shading")
{
    // CHK-274
    constexpr auto source = "struct link:\n    @position p: hpos4\n    @interpolate(.perspective, .sample) c: float4\n"
                            "@pixel struct target:\n    c: float4\n"
                            "@pixel fun ps(l: link) -> target:\n    return { c = l.c }\n";
    CHECK(reports_for(source).contains("feature-not-declared user:[ps] ps needs sample_rate_shading"));
    CHECK(reports_for(cc::string("require sample_rate_shading\n") + source) == "");
}

TEST("sgl check - a vertex member's @format names the bytes it reads, and they decode into its type")
{
    // CHK-275
    auto const with = [](cc::string_view member)
    { return reports_for(cc::string("@vertex struct vin:\n    p: float3\n    ") + member + "\n"); };
    CHECK(with("@format(.rgba8_unorm) c: float4") == "");
    CHECK(with("@format(.rgba8_uint) j: uint4") == "");
    CHECK(with("@format(.rgba8_unorm) c: float3")
          == "type-mismatch user:[format] @format(.rgba8_unorm) decodes into a float4, and the member is a float3\n");
    CHECK(with("@format(.bgra8) c: float4").contains("invalid-attribute-arguments"));
    // on a struct that is no vertex input, @format is no member's own
    CHECK(reports_for("struct plain:\n    @format(.rgba8_unorm) c: float4\n").contains("unsupported-yet"));
}

TEST("sgl check - a @pixel struct's depth and sample mask are outputs of their types, one each")
{
    // CHK-276
    auto const pixel = [](cc::string_view members)
    {
        return reports_for(cc::string("struct vout:\n    @position p: hpos4\n@pixel struct target:\n") + members
                           + "@pixel fun ps(v: vout) -> target:\n    return target()\n");
    };
    CHECK(reports_for("struct vout:\n    @position p: hpos4\n@pixel struct target:\n    c: float4\n    @depth d: "
                      "float\n"
                      "    @sample_mask m: uint\n@pixel fun ps(v: vout) -> target:\n"
                      "    return { c = float4(1.0, 1.0, 1.0, 1.0), d = 0.5, m = 0xff }\n")
          == "");
    CHECK(pixel("    @depth d: float4\n").contains("a @depth member is a float, and d is a float4"));
    CHECK(pixel("    @sample_mask m: int\n").contains("a @sample_mask member is a uint"));
    CHECK(pixel("    @depth(.sideways) d: float\n").contains("invalid-attribute-arguments"));
    CHECK(pixel("    @depth a: float\n    @depth b: float\n")
              .contains("a @pixel struct has at most one @depth member and one @sample_mask member"));
    // only a @pixel struct writes a pixel's outputs
    CHECK(reports_for("struct plain:\n    @depth d: float\n").contains("unsupported-yet"));
}

TEST("sgl check - only a pixel entry point may reach discard")
{
    // CHK-277: judged once the body is inlined, so a helper may hold one
    constexpr auto helper = "fun cut(a: float) -> float:\n    if a < 0.5 => discard\n    return a\n";
    CHECK(reports_for(cc::string(helper)
                      + "struct vout:\n    @position p: hpos4\n"
                        "@vertex fun vs(@vertex_index i: int) -> vout:\n"
                        "    return { p = hpos4(cut(0.25), 0.0, 0.0, 1.0) }\n")
          == "stage-not-allowed user:[discard] a vertex entry point reaches this, and only a pixel stage discards\n");
    CHECK(reports_for(cc::string(helper)
                      + "struct vout:\n    @position p: hpos4\n@pixel struct target:\n    c: float4\n"
                        "@pixel fun ps(v: vout) -> target:\n    return { c = float4(cut(v.p.x), 0.0, 0.0, 1.0) }\n")
          == "");
}

TEST("sgl check - a vertex stage's @layer is one uint the rasterizer reads, and needs vertex_layer_output")
{
    // CHK-419
    auto const layered = [](cc::string_view fields, cc::string_view require = "require vertex_layer_output\n\n")
    {
        return reports_for(cc::format("{}struct layered_out:\n    @position p: hpos4\n{}\n"
                                      "@vertex fun vs(@instance_index i: int) -> layered_out:\n"
                                      "    return {{ p = hpos4(0.0, 0.0, 0.0, 1.0), layer = i as uint }}\n",
                                      require, fields));
    };
    CHECK(layered("    @layer layer: uint\n") == "");
    CHECK(layered("    @layer layer: uint\n", "").contains("feature-not-declared user:[vs] vs needs vertex_layer_output"));
    CHECK(layered("    @layer layer: int\n").contains("the @layer field 'layer' of a @vertex fun is a uint"));
    CHECK(layered("    @layer @interpolate(.flat) layer: uint\n")
              .contains("the @layer member 'layer' is the rasterizer's, and is no interpolated value"));
    // a struct no rasterizer reads has no layer to pick
    CHECK(reports_for("@vertex struct vin:\n    @layer layer: uint\n\nstruct layered_out:\n    @position p: hpos4\n\n"
                      "@vertex fun vs(v: vin) -> layered_out:\n    return { p = hpos4(0.0, 0.0, 0.0, 1.0) }\n")
              .contains("reaches no rasterizer, so @layer means nothing on it"));
}
