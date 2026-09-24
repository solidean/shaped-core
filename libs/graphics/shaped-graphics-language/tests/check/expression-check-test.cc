#include "check-test-support.hh"

#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl_test;
using sgl::check::target_kind;

namespace
{
/// The reports of `lines` as the body of `fun f(v: vec3, c: float3, k: float) -> float`; `lines` is indented here.
cc::string body_reports(cc::string_view lines)
{
    auto source = cc::string("fun f(v: vec3, c: float3, k: float) -> float:\n");
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

/// The first expression of the user file that is spelled exactly `text`.
sgl::ast::expr_id find_expr(checked_sources const& s, cc::string_view text)
{
    for (auto i = isize(0); i < s.user_ast.exprs.size(); ++i)
        if (s.user.text_of(s.user.at(s.user_ast.exprs[i].form).where) == text)
            return sgl::ast::expr_id(i);
    return sgl::ast::expr_id::none;
}

constexpr auto pixel_edges = cc::string_view("struct pin:\n    @position position: hpos4\n    color: float3\n"
                                             "@pixel struct target:\n    color: float4\n");
} // namespace

TEST("sgl check - a body of lets and a return checks clean")
{
    CHECK(body_reports("let n = normalize v\nlet d = dot(n, v)\nreturn saturate d * k + 0.5\n") == "");
    CHECK(body_reports("return (k)\n") == "");
    CHECK(body_reports("let x : float = k\nreturn x\n") == "");
    CHECK(reports_for("fun f(k: float) -> float => k * 2.0\n") == "");
}

TEST("sgl check - overloads resolve by exact argument types")
{
    CHECK(body_reports("return dot(k, k)\n") == "no-matching-overload user:[dot(k, k)] dot(float, float)\n");
    CHECK(body_reports("return dot(v)\n") == "no-matching-overload user:[dot(v)] dot(vec3)\n");
    CHECK(body_reports("return saturate()\n") == "no-matching-overload user:[saturate()] saturate()\n");

    // A second declaration with the same parameter types in one scope is no error by itself; the call cannot choose.
    CHECK(reports_for("fun g(v: vec3) -> float => v.x\nfun g(v: vec3) -> float => v.y\nfun f(v: vec3) -> float:\n"
                      "    return g(v)\n")
          == "ambiguous-overload user:[g(v)] g(vec3) has 2 candidates\n");

    // CHK-192: across the two scopes the user file's wins, so a prelude that gains its signature later breaks nothing
    auto const shadowed = check_sources(read_prelude(), "fun dot(x: vec3, y: vec3) -> float => 7.0\n"
                                                        "fun f(v: vec3) -> float:\n"
                                                        "    return dot(v, v)\n");
    CHECK(reports_of(shadowed) == "");
    auto const chosen = shadowed.tables().target_at(find_expr(shadowed, "dot(v, v)"));
    CHECK(chosen.kind == target_kind::overload);
    CHECK(shadowed.module.at(chosen.symbol).file == shadowed.user_file());

    // An overload on other types takes nothing away, and each call records the one it chose.
    auto const checked = check_sources(read_prelude(), "fun dot(a: pos3, b: pos3) -> float => a.x * b.x\n"
                                                       "fun f(v: vec3, c: pos3) -> float:\n"
                                                       "    return dot(v, v) + dot(c, c)\n");
    CHECK(reports_of(checked) == "");
    auto const& tables = checked.tables();
    auto const on_vec3 = tables.target_at(find_expr(checked, "dot(v, v)"));
    auto const on_pos3 = tables.target_at(find_expr(checked, "dot(c, c)"));
    CHECK(on_vec3.kind == target_kind::overload);
    CHECK(on_pos3.kind == target_kind::overload);
    CHECK(checked.module.at(on_vec3.symbol).file == 0);
    CHECK(checked.module.at(on_pos3.symbol).file == checked.user_file());
}

TEST("sgl check - an operator is a function found through its spelling")
{
    CHECK(body_reports("return k * k + k\n") == "");
    CHECK(body_reports("return v * v\n") == "no-matching-overload user:[v * v] operator *(vec3, vec3)\n");
    CHECK(body_reports("return k / v\n") == "no-matching-overload user:[k / v] operator /(float, vec3)\n");
    CHECK(body_reports("let p = pos3(k, k, k)\nreturn -p\n") == "no-matching-overload user:[-p] operator -(pos3)\n");
    // a position plus a position means nothing, and the prelude says so by leaving it out
    CHECK(body_reports("let p = pos3(k, k, k)\nreturn length(p + p)\n")
          == "no-matching-overload user:[p + p] operator +(pos3, pos3)\n");
    CHECK(body_reports("let p = pos3(k, k, k)\nreturn length((p + v) - p)\n") == "");
    // a prefix operator is a function of one parameter
    CHECK(body_reports("return -k + -(k * k)\n") == "");

    auto const checked = check_sources(read_prelude(), "fun f(c: float3, k: float) -> float3:\n    return c * k\n");
    CHECK(reports_of(checked) == "");
    auto const chosen = checked.tables().target_at(find_expr(checked, "c * k"));
    REQUIRE(chosen.kind == target_kind::overload);
    CHECK(checked.module.at(chosen.symbol).name == "scale_color");
    CHECK(checked.module.at(chosen.symbol).operator_spelling == "*");
}

TEST("sgl check - juxtaposition and parens are one call")
{
    auto const checked = check_sources(read_prelude(), "fun f(v: vec3) -> vec3:\n"
                                                       "    let a = normalize v\n"
                                                       "    let b = normalize(v)\n"
                                                       "    return a\n");
    CHECK(reports_of(checked) == "");
    auto const& tables = checked.tables();
    CHECK(tables.target_at(find_expr(checked, "normalize v")) == tables.target_at(find_expr(checked, "normalize(v)")));
    CHECK(tables.type_at(find_expr(checked, "normalize v")) == tables.type_at(find_expr(checked, "normalize(v)")));
}

TEST("sgl check - a call of a function of the program resolves like any other, overloads included")
{
    CHECK(reports_for("fun half(x: float) -> float => x * 0.5\nfun f(k: float) -> float:\n    return half k\n") == "");
    CHECK(reports_for("fun half(x: float) -> float => x * 0.5\nfun f(v: vec3) -> float:\n    return half v\n")
          == "no-matching-overload user:[half v] half(vec3)\n");

    auto const checked = check_sources(read_prelude(), "fun half(x: float) -> float => x * 0.5\n"
                                                       "fun half(v: vec3) -> vec3 => v * 0.5\n"
                                                       "fun f(v: vec3, k: float) -> float:\n"
                                                       "    return dot(half(v), v) * half(k)\n");
    CHECK(reports_of(checked) == "");
    auto const& tables = checked.tables();
    auto const on_vec3 = tables.target_at(find_expr(checked, "half(v)"));
    auto const on_float = tables.target_at(find_expr(checked, "half(k)"));
    REQUIRE(on_vec3.kind == target_kind::overload);
    REQUIRE(on_float.kind == target_kind::overload);
    CHECK(on_vec3.symbol != on_float.symbol);
    CHECK(checked.module.name_of(tables.type_at(find_expr(checked, "half(v)"))) == "vec3");
}

TEST("sgl check - a struct's constructor takes its fields in order, and a splat spreads a struct")
{
    CHECK(body_reports("let a = vec3(k, k, k)\nreturn dot(a, a)\n") == "");
    CHECK(body_reports("let a = float4(..c, k)\nreturn k\n") == "");
    CHECK(body_reports("let a = float4(k, ..c)\nreturn k\n") == "");
    CHECK(body_reports("let a = vec3(..c)\nreturn k\n") == "");

    CHECK(body_reports("let a = vec3(k, k)\nreturn k\n")
          == "no-matching-overload user:[vec3(k, k)] vec3(float, float), and the constructor is vec3(float, float, "
             "float)\n");
    CHECK(body_reports("let a = float4(..c)\nreturn k\n")
          == "no-matching-overload user:[float4(..c)] float4(float, float, float), and the constructor is "
             "float4(float, "
             "float, float, float)\n");
    CHECK(body_reports("let a = float(k)\nreturn k\n")
          == "no-matching-overload user:[float(k)] float is opaque and has no constructor\n");
    CHECK(body_reports("let a = vec3(..k, k, k)\nreturn k\n")
          == "type-mismatch user:[..k] float has no fields a splat could spread\n");
    CHECK(body_reports("return dot(..v, v)\n") == "unsupported-yet user:[..v] a splat outside a constructor call\n");
}

TEST("sgl check - names, members and what stands for the wrong thing")
{
    CHECK(body_reports("return nope\n") == "unknown-name user:[nope] nope\n");
    CHECK(body_reports("return nope(k)\n") == "unknown-name user:[nope] nope\n");
    CHECK(body_reports("return v.w\n") == "unknown-member user:[w] vec3 has no member w\n");
    CHECK(body_reports("return k.x\n") == "unknown-member user:[x] float has no member x\n");
    CHECK(reports_for("binding constants:\n    m: mat4\nfun f(k: float) -> float:\n    return constants(k)\n")
          == "wrong-kind-of-name user:[constants] constants is a binding, and a call needs a function or a struct\n");
    CHECK(body_reports("let t = vec3\nreturn k\n") == "unsupported-yet user:[vec3] a type as a value\n");
    CHECK(body_reports("let t = dot\nreturn k\n") == "unsupported-yet user:[dot] a function as a value\n");
}

TEST("sgl check - the error type is silent: one mistake, one diagnostic")
{
    CHECK(body_reports("let a = nope\nlet b = normalize a\nlet e = dot(b, v) * k\nreturn e\n")
          == "unknown-name user:[nope] nope\n");
    CHECK(body_reports("return vec3(nope, k, k).x\n") == "unknown-name user:[nope] nope\n");
}

TEST("sgl check - a binding member is reachable only through the function's binding list")
{
    auto const binding = cc::string("binding frame:\n    exposure: float\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    return frame.exposure * k\n") == "");
    CHECK(reports_for(binding + "fun f(k: float) -> float:\n    return frame.exposure * k\n")
          == "binding-not-listed user:[frame] frame is not in the binding list of f\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    return frame.gamma\n")
          == "unknown-member user:[gamma] the binding frame has no member gamma\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    let g = frame\n    return k\n")
          == "unsupported-yet user:[frame] a binding as a value\n");
}

TEST("sgl check - a number literal with a dot or an exponent is a float, and nothing else is carried")
{
    CHECK(body_reports("return 0.5 + -0.4 + 1e3 + 2.5e-3 + 1. + 1'000.0\n") == "");
    CHECK(body_reports("return 1\n") == "type-mismatch user:[1] expected float, got int\n");
    CHECK(body_reports("let i = 1'000 + -3\nreturn k\n") == "");
    CHECK(body_reports("let i = 3'000'000'000\nreturn k\n")
          == "unsupported-yet user:[3'000'000'000] an integer literal that does not fit an int\n");
    CHECK(body_reports("return 0.5f32\n")
          == "unsupported-yet user:[0.5f32] a number literal with a prefix, a suffix or a p exponent\n");
    CHECK(body_reports("return 0xff\n")
          == "unsupported-yet user:[0xff] a number literal with a prefix, a suffix or a p exponent\n");
    CHECK(body_reports("let s = \"text\"\nreturn k\n") == "unsupported-yet user:[\"text\"] a string literal\n");

    // The literal's type is the prelude's `float`, so a module without one says so.
    CHECK(reports_of(check_sources("", "@builtin struct mat4\nfun f(m: mat4) -> mat4:\n    let a = 1.0\n    return "
                                       "m\n"))
          == "unknown-name user:[1.0] float, which the prelude must declare @builtin\n");
}

TEST("sgl check - a returned object converts structurally: every field once, types equal")
{
    auto const head = cc::string("struct pair:\n    a: float\n    b: vec3\nfun f(k: float, v: vec3) -> pair:\n");
    CHECK(reports_for(head + "    return { a = k, b = v }\n") == "");
    CHECK(reports_for(head + "    return { b = v, a = k }\n") == "");
    CHECK(reports_for(head + "    return { a = k }\n") == "missing-field user:[{ a = k }] b\n");
    CHECK(reports_for(head + "    return { a = k, b = v, c = k }\n") == "unknown-field user:[c] pair has no field c\n");
    CHECK(reports_for(head + "    return { a = k, a = k, b = v }\n") == "duplicate-field user:[a] a\n");
    CHECK(reports_for(head + "    return { a = v, b = v }\n") == "type-mismatch user:[v] a is float, got vec3\n");
    CHECK(reports_for(head + "    return k\n") == "type-mismatch user:[k] expected pair, got float\n");
    CHECK(reports_for(head + "    let p = { a = k, b = v }\n    return p\n")
          == "unsupported-yet user:[{ a = k, b = v }] an object with no struct to convert to\n");

    // an object inside an object converts to the field's struct
    CHECK(reports_for(head
                      + "struct outer:\n    p: pair\n    k: float\nfun g(k: float, v: vec3) -> outer:\n"
                        "    return { p = { a = k, b = v }, k = k }\n")
          == "");
}

TEST("sgl check - let introduces an immutable local, in order")
{
    CHECK(body_reports("let a = b\nlet b = k\nreturn k\n") == "unknown-name user:[b] b\n");
    // CHK-53: shadowing a local or a parameter is legal
    CHECK(body_reports("let a = k\nlet a = k\nreturn k\n") == "");
    CHECK(body_reports("let k = 1.0\nreturn k\n") == "");
    CHECK(body_reports("let x : vec3 = k\nreturn k\n") == "type-mismatch user:[k] expected vec3, got float\n");
    CHECK(body_reports("let mut a = k\nreturn a\n") == "");
    CHECK(body_reports("let a : float\nreturn k\n") == "unsupported-yet user:[let a : float] a let without a value\n");
    CHECK(body_reports("let (a, b) = k\nreturn k\n") == "unsupported-yet user:[(a, b)] a pattern in let\n");
}

TEST("sgl check - a local or a parameter shadows a module-level name, and hides it wherever it stands")
{
    // CHK-54: the value still sees what the local hides
    CHECK(body_reports("let dot = k\nreturn dot\n") == "");
    CHECK(body_reports("let length = length v\nreturn length\n") == "");
    // one namespace: behind the local, the name is no function and no type
    CHECK(body_reports("let dot = k\nreturn dot v v\n") == "unsupported-yet user:[dot] a call of a local value\n");
    CHECK(body_reports("let float = k\nlet a : float = k\nreturn k\n")
          == "wrong-kind-of-name user:[float] float is a local, and a type stands here\n");
    // a literal is of the prelude's type whatever a local is named
    CHECK(body_reports("let float = 1.0\nlet int = 2\nreturn float\n") == "");
    // an inner block's local hides the name only up to its end
    CHECK(body_reports("if k > 0.0:\n    let dot = k\n    return dot\nreturn dot v v\n") == "");

    CHECK(reports_for("fun g(dot: float) -> float:\n    return dot\n") == "");
    CHECK(reports_for("binding frame:\n    exposure: float\nfun g(k: float) -> float:\n    let frame = k\n    return "
                      "frame\n")
          == "");
    // a parameter's type is resolved before the parameters are in scope, so it may be named after its type
    CHECK(reports_for("struct light:\n    x: float\nfun g(light: light) -> float:\n    return light.x\n") == "");
}

TEST("sgl check - statements and expressions the tracer does not carry")
{
    CHECK(body_reports("for i in v:\n    return k\nreturn k\n")
          == "unsupported-yet user:[v] a for over anything but an int range\n");
    CHECK(body_reports("for i in 0 ..= 3:\n    return k\nreturn k\n")
          == "unsupported-yet user:[0 ..= 3] a for over a range that is not `..<`\n");
    CHECK(body_reports("assert k > 0.0, \"positive\"\nreturn k\n").contains("unsupported-yet user:[assert"));
    // a call may be written for its effect, so its value may be dropped; any other expression has none to be written for
    CHECK(body_reports("saturate k\nreturn k\n") == "");
    CHECK(body_reports("k\nreturn k\n") == "unsupported-yet user:[k] an expression statement\n");
    CHECK(body_reports("let h = x => x\nreturn k\n") == "unsupported-yet user:[x => x] a lambda\n");
    CHECK(body_reports("let t = (k, k)\nreturn k\n") == "unsupported-yet user:[(k, k)] a tuple\n");
    CHECK(body_reports("let t = k as vec3\nreturn k\n") == "unsupported-yet user:[k as vec3] as\n");
    CHECK(body_reports("fun g(x: float) -> float => x\nreturn k\n")
          == "unsupported-yet user:[fun g(x: float) -> float => x] a declaration inside a function\n");
}

TEST("sgl check - the side tables name what an editor asks for")
{
    auto const checked = check_sources(read_prelude(), cc::string(pixel_edges)
                                                           + "@pixel fun ps(p: pin) -> target:\n"
                                                             "    let lit = p.color * 0.5\n"
                                                             "    return { color = float4(..lit, 1.0) }\n");
    CHECK(reports_of(checked) == "");
    auto const& m = checked.module;
    auto const& tables = checked.tables();

    auto const member = find_expr(checked, "p.color");
    CHECK(m.name_of(tables.type_at(member)) == "float3");
    CHECK(tables.target_at(member).kind == target_kind::field);
    CHECK(m.at(tables.target_at(member).symbol).name == "pin");
    CHECK(tables.target_at(member).index == 1);

    CHECK(tables.target_at(find_expr(checked, "p")).kind == target_kind::parameter);
    // the first `lit` is the pattern of the let, which names itself
    CHECK(tables.target_at(find_expr(checked, "lit")).kind == target_kind::local);
    CHECK(tables.target_at(find_expr(checked, "float4(..lit, 1.0)")).kind == target_kind::constructor);
    CHECK(m.name_of(tables.type_at(find_expr(checked, "1.0"))) == "float");
    // a type position has the type it names
    CHECK(m.name_of(tables.type_at(find_expr(checked, "hpos4"))) == "hpos4");
    CHECK(tables.target_at(find_expr(checked, "hpos4")).kind == target_kind::symbol);
}

TEST("sgl check - the flat tree spreads a splat, and evaluates a splatted value once")
{
    auto const head = cc::string(pixel_edges) + "@pixel fun ps(p: pin) -> target:\n";

    auto const local = check_sources(read_prelude(),
                                     head + "    let lit = p.color * 0.5\n    return { color = float4(..lit, 1.0) }\n");
    CHECK(reports_of(local) == "");
    CHECK(sgl::check::dump_entry_points(local.module)
          == "(entry pixel ps (p : pin) -> target\n"
             "  (let lit : float3 = (call scale_color (member (local p : pin) color : float3) (lit 0.5 : float) : "
             "float3))\n"
             "  (return (construct\n"
             "    (construct (member (local lit : float3) x : float) (member (local lit : float3) y : float) "
             "(member (local lit : float3) z : float) (lit 1.0 : float) : float4) : target)))\n");

    auto const temporary = check_sources(
        read_prelude(), head + "    let splat = 1.0\n    return { color = float4(..(p.color * splat), 1.0) }\n");
    CHECK(reports_of(temporary) == "");
    // The value is no local, so it gets one; its name is minted, and the program's own `splat` keeps its name.
    // It is bound where its first member stands, so nothing is evaluated earlier than the source says.
    CHECK(sgl::check::dump_entry_points(temporary.module)
          == "(entry pixel ps (p : pin) -> target\n"
             "  (let splat : float = (lit 1.0 : float))\n"
             "  (return (construct\n"
             "    (construct (member (block $splat\n"
             "      (let:temporary splat_1 : float3 = (call scale_color (member (local p : pin) color : float3) "
             "(local splat : float) : float3))\n"
             "      (leave $splat (local splat_1 : float3)) : float3) x : float) "
             "(member (local splat_1 : float3) y : float) "
             "(member (local splat_1 : float3) z : float) (lit 1.0 : float) : float4) : target)))\n");

    // Core, it is the `let` in front that it always was.
    auto const& m = temporary.module;
    CHECK(sgl::check::dump_entry_point(m, sgl::check::legalize(m, m.entry_points[0]))
          == "(entry pixel ps (p : pin) -> target\n"
             "  (let splat : float = (lit 1.0 : float))\n"
             "  (let:temporary splat_1 : float3 = (call scale_color (member (local p : pin) color : float3) "
             "(local splat : float) : float3))\n"
             "  (return (construct\n"
             "    (construct (member (local splat_1 : float3) x : float) (member (local splat_1 : float3) y : float) "
             "(member (local splat_1 : float3) z : float) (lit 1.0 : float) : float4) : target)))\n");
}

TEST("sgl check - a flat node keeps its AST node, and no call site while nothing is inlined")
{
    auto const checked = check_sources(read_prelude(), cc::string(pixel_edges)
                                                           + "@pixel fun ps(p: pin) -> target:\n"
                                                             "    return { color = float4(..p.color, 1.0) }\n");
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& e = checked.module.entry_points[0];
    for (auto const& x : e.exprs)
    {
        CHECK(x.from.file == checked.user_file());
        CHECK(sgl::ast::is_valid(x.from.expr));
        CHECK(x.inlined_through.empty());
        CHECK(!x.node.is<sgl::check::flat_invalid>());
    }
    CHECK(e.call_sites.empty());
    CHECK(e.entry_stage == sgl::check::stage::pixel);
    CHECK(checked.module.name_of(e.input) == "pin");
    CHECK(checked.module.name_of(e.result) == "target");
}

TEST("sgl check - name_mint never hands out a name twice")
{
    auto names = sgl::check::name_mint();
    CHECK(names.reserve("main_ps"));
    CHECK(!names.reserve("main_ps"));
    CHECK(names.mint("n") == "n");
    CHECK(names.mint("n") == "n_1");
    CHECK(names.reserve("n_2"));
    CHECK(names.mint("n") == "n_3");
    CHECK(names.mint("main_ps") == "main_ps_1");
    CHECK(names.mint("") == "_");
    CHECK(names.is_taken("n_3"));
}
