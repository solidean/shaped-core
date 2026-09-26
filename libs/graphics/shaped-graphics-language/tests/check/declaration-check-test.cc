#include "check-test-support.hh"

using namespace sgl_test;
using sgl::check::symbol_state;

TEST("sgl check - a @builtin is keyed by its name, and an unknown name is unknown-builtin")
{
    CHECK(reports_for("@builtin struct quaternion\n") == "unknown-builtin user:[quaternion] quaternion\n");
    CHECK(reports_for("@builtin fun frobnicate(x: float) -> float\n") == "unknown-builtin user:[frobnicate] frobnicate\n");

    // The name of a builtin function declares no builtin struct, and the other way round.
    auto const swapped = check_sources("", "@builtin struct dot\n@builtin fun mat4(v: dot) -> dot\n");
    CHECK(reports_of(swapped) == "unknown-builtin user:[dot] dot\nunknown-builtin user:[mat4] mat4\n");

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

TEST("sgl check - a name is declared once, unless every declaration of it is a function")
{
    CHECK(reports_for("struct a:\n    x: float\nstruct a:\n    y: float\n") == "duplicate-declaration user:[a] a\n");
    CHECK(reports_for("struct a:\n    x: float\nfun a(k: float) -> float => k\n") == "duplicate-declaration user:[a] a\n");
    CHECK(reports_for("struct a:\n    x: float\n    x: float\n") == "duplicate-declaration user:[x] x\n");
    CHECK(reports_for("@builtin fun dot(a: float3, a: float3) -> float\n") == "duplicate-declaration user:[a] a\n");

    // two functions of one name are an overload set
    CHECK(reports_for("@builtin fun dot(a: float3, b: float3) -> float\n") == "");
}

TEST("sgl check - the prelude is a scope around the user file's, which may shadow its names")
{
    // the user file's struct is the one its lookups find: the prelude's vec3 has no member `weight`
    CHECK(reports_for("struct vec3:\n    weight: float\nfun g(v: vec3) -> float:\n    return v.weight\n") == "");
    CHECK(reports_for("binding dot:\n    x: float\n") == "");
    // a function hides a struct of the prelude, and a struct the prelude's functions, unless both are functions
    CHECK(reports_for("fun vec3(k: float) -> float => k\nfun g(k: float) -> float:\n    return vec3(k)\n") == "");
    CHECK(reports_for("struct dot:\n    a: float\nfun g(d: dot) -> float:\n    return d.a\n") == "");
    // what the checker needs of the prelude by name stays the prelude's: a for runs over its int
    CHECK(reports_for("struct int:\n    a: float\nfun g(k: float) -> float:\n    for i in 0 ..< 3:\n        return k\n"
                      "    return k\n")
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
    auto const outer = m.symbols[m.symbols.size() - 2];
    auto const inner = m.symbols[m.symbols.size() - 1];
    CHECK(outer.name == "outer");
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
    auto const& a = m.at(m.symbols.back().type);
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
    CHECK(reports_for("const a = b\nconst b = a\n").contains("dependency-cycle"));
    CHECK(reports_for("const k = 1.0\nstruct a:\n    x: k\n")
          == "wrong-kind-of-name user:[k] k is a const, and a type stands here\n");
    // true and false are consts of the prelude, and a case over a bool that names both is exhaustive (CHK-221)
    CHECK(reports_for("fun f(b: bool) -> float:\n    if b == true => return 1.0\n    return case b:\n"
                      "        true => 0.5\n        false => 0.0\n")
          == "");
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
    CHECK(reports_for("const k = 2.0 * 1.0\n")
          == "unsupported-yet user:[2.0 * 1.0] a const whose value is no literal, no enum case and no const\n");
    CHECK(reports_for("type color = float3\n") == "unsupported-yet user:[type color = float3] type alias\n");
    CHECK(reports_for("use brdf\n") == "unsupported-yet user:[use brdf] use\n");
    CHECK(reports_for("sampler s:\n    filter = .linear\n") == "unsupported-yet user:[sampler s:] sampler\n");
    CHECK(reports_for("fun id[T](x: T) -> T => x\n").starts_with("unsupported-yet user:[id] a generic function\n"));
    CHECK(reports_for("struct a:\n    x: float\n    len => x\n") == "unsupported-yet user:[len => x] a property\n");
    CHECK(reports_for("struct a:\n    x: float = 1.0\n") == "unsupported-yet user:[1.0] a default value\n");
    CHECK(reports_for("binding b = constants\n") == "unsupported-yet user:[constants] a binding composition\n");
    // CHK-25: a format is no type, so a texture takes what it samples to and an image takes a format as a case.
    CHECK(reports_for("binding b:\n    t: texture_2d[rgba8]\n") == "unknown-name user:[rgba8] rgba8\n");
    CHECK(reports_for("@format(rgba8) struct a:\n    x: float\n")
          == "unsupported-yet user:[format] the attribute @format on a struct\n");

    // an unsupported declaration still owns its name, so a use of it is silent
    CHECK(reports_for("type color = float3\nstruct a:\n    x: color\n")
          == "unsupported-yet user:[type color = float3] type alias\n");
    // the module line is accepted
    CHECK(reports_for("module cube\nstruct a:\n    x: float\n") == "");
}

TEST("sgl check - a function needs a body unless it is @builtin, and a generic one fails as a whole")
{
    CHECK(reports_for("fun f(x: float) -> float\n") == "expected-body user:[f] f\n");

    auto const checked = check_sources(read_prelude(), "fun id[T](x: float) -> float => x\n");
    CHECK(checked.module.symbols.back().state == symbol_state::failed);
    CHECK(sgl::check::dump(checked.module).ends_with("(fun id failed)\n"));
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
    REQUIRE(checked.module.bindings.size() == 2);
    CHECK(checked.module.bindings[0].is_inline);
    CHECK(!checked.module.bindings[1].is_inline);
}

TEST("sgl check - the binding list is checked: every entry is a bare name of a binding")
{
    CHECK(reports_for("fun f(x: float){nope} -> float => x\n") == "unknown-name user:[nope] nope\n");
    CHECK(reports_for("fun f(x: float){vec3} -> float => x\n") == "wrong-kind-of-name user:[vec3] vec3 is no binding\n");
    CHECK(reports_for("binding b:\n    k: float\nfun f(x: float){b = b} -> float => x\n")
          == "unsupported-yet user:[b = b] a binding entry that is not a bare name\n");
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
          == "invalid-entry-point user:[vs] the parameter of a @vertex fun is a @vertex struct\n");
    CHECK(entry_reports("@vertex fun vs(v: vin, w: vin) -> vout:\n    return { p = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] an entry point takes one struct parameter\n");
    CHECK(entry_reports("@vertex fun vs(v: vin) -> plain:\n    return { p = v.p }\n")
          == "invalid-entry-point user:[vs] a @vertex fun returns a struct with exactly one @position field\n");
    CHECK(entry_reports("@vertex fun vs(v: vin) -> two:\n    return { a = hpos4(..v.p, 1.0), b = hpos4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] a @vertex fun returns a struct with exactly one @position field\n");
    CHECK(entry_reports("@vertex fun vs(v: vin) -> wrong:\n    return { a = float4(..v.p, 1.0) }\n")
          == "invalid-entry-point user:[vs] the @position field of a @vertex fun is an hpos4\n");

    CHECK(entry_reports("@pixel fun ps(v: vout) -> target:\n    return { c = float4(..v.p) }\n") == "");
    CHECK(entry_reports("@pixel fun ps(v: vout) -> plain:\n    return { p = pos3(1.0, 1.0, 1.0) }\n")
          == "invalid-entry-point user:[ps] a @pixel fun returns a @pixel struct\n");
    CHECK(entry_reports("@pixel fun ps(v: float) -> target:\n    return { c = float4(v, v, v, v) }\n")
          == "invalid-entry-point user:[ps] the parameter of an entry point is a struct with fields\n");
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
