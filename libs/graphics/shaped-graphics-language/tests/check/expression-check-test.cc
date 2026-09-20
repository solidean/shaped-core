#include "check-test-support.hh"

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
    CHECK(body_reports("return dot(k, k)\n") == "no-matching-overload 1:[dot(k, k)] dot(float, float)\n");
    CHECK(body_reports("return dot(v)\n") == "no-matching-overload 1:[dot(v)] dot(vec3)\n");
    CHECK(body_reports("return saturate()\n") == "no-matching-overload 1:[saturate()] saturate()\n");

    // A second declaration with the same parameter types is no error by itself; the call cannot choose.
    CHECK(reports_for("@builtin fun dot(x: vec3, y: vec3) -> float\nfun f(v: vec3) -> float:\n    return dot(v, v)\n")
          == "ambiguous-overload 1:[dot(v, v)] dot(vec3, vec3) has 2 candidates\n");

    // An overload on other types takes nothing away, and each call records the one it chose.
    auto const checked = check_sources(read_prelude(), "@builtin fun dot(a: float3, b: float3) -> float\n"
                                                       "fun f(v: vec3, c: float3) -> float:\n"
                                                       "    return dot(v, v) + dot(c, c)\n");
    CHECK(reports_of(checked) == "");
    auto const& tables = checked.module.files[1];
    auto const on_vec3 = tables.target_at(find_expr(checked, "dot(v, v)"));
    auto const on_float3 = tables.target_at(find_expr(checked, "dot(c, c)"));
    CHECK(on_vec3.kind == target_kind::overload);
    CHECK(on_float3.kind == target_kind::overload);
    CHECK(checked.module.at(on_vec3.symbol).file == 0);
    CHECK(checked.module.at(on_float3.symbol).file == 1);
}

TEST("sgl check - an operator is a function found through its spelling")
{
    CHECK(body_reports("return k * k + k\n") == "");
    CHECK(body_reports("return v * k\n") == "no-matching-overload 1:[v * k] operator *(vec3, float)\n");
    CHECK(body_reports("return k / k\n") == "no-matching-overload 1:[k / k] operator /(float, float)\n");
    CHECK(body_reports("return -k\n") == "no-matching-overload 1:[-k] operator -(float)\n");

    auto const checked = check_sources(read_prelude(), "fun f(c: float3, k: float) -> float3:\n    return c * k\n");
    CHECK(reports_of(checked) == "");
    auto const chosen = checked.module.files[1].target_at(find_expr(checked, "c * k"));
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
    auto const& tables = checked.module.files[1];
    CHECK(tables.target_at(find_expr(checked, "normalize v")) == tables.target_at(find_expr(checked, "normalize(v)")));
    CHECK(tables.type_at(find_expr(checked, "normalize v")) == tables.type_at(find_expr(checked, "normalize(v)")));
}

TEST("sgl check - a call of a function that is no builtin resolves, types, and waits for the inliner")
{
    CHECK(reports_for("fun half(x: float) -> float => x * 0.5\nfun f(k: float) -> float:\n    return half k\n")
          == "unsupported-yet 1:[half k] a call of a function that is no @builtin, which needs the inliner\n");
}

TEST("sgl check - a struct's constructor takes its fields in order, and a splat spreads a struct")
{
    CHECK(body_reports("let a = vec3(k, k, k)\nreturn dot(a, a)\n") == "");
    CHECK(body_reports("let a = float4(..c, k)\nreturn k\n") == "");
    CHECK(body_reports("let a = float4(k, ..c)\nreturn k\n") == "");
    CHECK(body_reports("let a = vec3(..c)\nreturn k\n") == "");

    CHECK(body_reports("let a = vec3(k, k)\nreturn k\n")
          == "no-matching-overload 1:[vec3(k, k)] vec3(float, float), and the constructor is vec3(float, float, "
             "float)\n");
    CHECK(body_reports("let a = float4(..c)\nreturn k\n")
          == "no-matching-overload 1:[float4(..c)] float4(float, float, float), and the constructor is float4(float, "
             "float, float, float)\n");
    CHECK(body_reports("let a = float(k)\nreturn k\n")
          == "no-matching-overload 1:[float(k)] float is opaque and has no constructor\n");
    CHECK(body_reports("let a = vec3(..k, k, k)\nreturn k\n")
          == "type-mismatch 1:[..k] float has no fields a splat could spread\n");
    CHECK(body_reports("return dot(..v, v)\n") == "unsupported-yet 1:[..v] a splat outside a constructor call\n");
}

TEST("sgl check - names, members and what stands for the wrong thing")
{
    CHECK(body_reports("return nope\n") == "unknown-name 1:[nope] nope\n");
    CHECK(body_reports("return nope(k)\n") == "unknown-name 1:[nope] nope\n");
    CHECK(body_reports("return v.w\n") == "unknown-member 1:[w] vec3 has no member w\n");
    CHECK(body_reports("return k.x\n") == "unknown-member 1:[x] float has no member x\n");
    CHECK(reports_for("binding constants:\n    m: mat4\nfun f(k: float) -> float:\n    return constants(k)\n")
          == "wrong-kind-of-name 1:[constants] constants is a binding, and a call needs a function or a struct\n");
    CHECK(body_reports("let t = vec3\nreturn k\n") == "unsupported-yet 1:[vec3] a type as a value\n");
    CHECK(body_reports("let t = dot\nreturn k\n") == "unsupported-yet 1:[dot] a function as a value\n");
}

TEST("sgl check - the error type is silent: one mistake, one diagnostic")
{
    CHECK(body_reports("let a = nope\nlet b = normalize a\nlet e = dot(b, v) * k\nreturn e\n")
          == "unknown-name 1:[nope] nope\n");
    CHECK(body_reports("return vec3(nope, k, k).x\n") == "unknown-name 1:[nope] nope\n");
}

TEST("sgl check - a binding member is reachable only through the function's binding list")
{
    auto const binding = cc::string("binding frame:\n    exposure: float\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    return frame.exposure * k\n") == "");
    CHECK(reports_for(binding + "fun f(k: float) -> float:\n    return frame.exposure * k\n")
          == "binding-not-listed 1:[frame] frame is not in the binding list of f\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    return frame.gamma\n")
          == "unknown-member 1:[gamma] the binding frame has no member gamma\n");
    CHECK(reports_for(binding + "fun f(k: float){frame} -> float:\n    let g = frame\n    return k\n")
          == "unsupported-yet 1:[frame] a binding as a value\n");
}

TEST("sgl check - a number literal with a dot or an exponent is a float, and nothing else is carried")
{
    CHECK(body_reports("return 0.5 + -0.4 + 1e3 + 2.5e-3 + 1. + 1'000.0\n") == "");
    CHECK(body_reports("return 1\n") == "unsupported-yet 1:[1] an integer literal, which the pass does not type yet\n");
    CHECK(body_reports("return 0.5f32\n")
          == "unsupported-yet 1:[0.5f32] a number literal with a prefix, a suffix or a p exponent\n");
    CHECK(body_reports("return 0xff\n")
          == "unsupported-yet 1:[0xff] a number literal with a prefix, a suffix or a p exponent\n");
    CHECK(body_reports("let s = \"text\"\nreturn k\n") == "unsupported-yet 1:[\"text\"] a string literal\n");

    // The literal's type is the prelude's `float`, so a module without one says so.
    CHECK(reports_of(check_sources("", "@builtin struct mat4\nfun f(m: mat4) -> mat4:\n    let a = 1.0\n    return "
                                       "m\n"))
          == "unknown-name 1:[1.0] float, which the prelude must declare @builtin\n");
}

TEST("sgl check - a returned object converts structurally: every field once, types equal")
{
    auto const head = cc::string("struct pair:\n    a: float\n    b: vec3\nfun f(k: float, v: vec3) -> pair:\n");
    CHECK(reports_for(head + "    return { a = k, b = v }\n") == "");
    CHECK(reports_for(head + "    return { b = v, a = k }\n") == "");
    CHECK(reports_for(head + "    return { a = k }\n") == "missing-field 1:[{ a = k }] b\n");
    CHECK(reports_for(head + "    return { a = k, b = v, c = k }\n") == "unknown-field 1:[c] pair has no field c\n");
    CHECK(reports_for(head + "    return { a = k, a = k, b = v }\n") == "duplicate-field 1:[a] a\n");
    CHECK(reports_for(head + "    return { a = v, b = v }\n") == "type-mismatch 1:[v] a is float, got vec3\n");
    CHECK(reports_for(head + "    return k\n") == "type-mismatch 1:[k] expected pair, got float\n");
    CHECK(reports_for(head + "    let p = { a = k, b = v }\n    return p\n")
          == "unsupported-yet 1:[{ a = k, b = v }] an object with no struct to convert to\n");

    // an object inside an object converts to the field's struct
    CHECK(reports_for(head
                      + "struct outer:\n    p: pair\n    k: float\nfun g(k: float, v: vec3) -> outer:\n"
                        "    return { p = { a = k, b = v }, k = k }\n")
          == "");
}

TEST("sgl check - let introduces an immutable local, in order")
{
    CHECK(body_reports("let a = b\nlet b = k\nreturn k\n") == "unknown-name 1:[b] b\n");
    CHECK(body_reports("let a = k\nlet a = k\nreturn k\n") == "duplicate-declaration 1:[a] a\n");
    CHECK(body_reports("let k = 1.0\nreturn k\n") == "duplicate-declaration 1:[k] k\n");
    CHECK(body_reports("let x : vec3 = k\nreturn k\n") == "type-mismatch 1:[k] expected vec3, got float\n");
    CHECK(body_reports("let dot = k\nreturn k\n") == "unsupported-yet 1:[dot] a local that shadows a module-level name\n");
    CHECK(body_reports("let mut a = k\nreturn a\n") == "unsupported-yet 1:[let mut a = k] let mut\n");
    CHECK(body_reports("let (a, b) = k\nreturn k\n") == "unsupported-yet 1:[(a, b)] a pattern in let\n");
}

TEST("sgl check - statements and expressions the tracer does not carry")
{
    CHECK(body_reports("if k > k => return k\nreturn k\n").contains("unsupported-yet 1:[if k > k => return k] if\n"));
    CHECK(body_reports("let a = k\na = k\nreturn a\n") == "unsupported-yet 1:[a = k] assignment\n");
    CHECK(body_reports("for i in v:\n    return k\nreturn k\n").starts_with("unsupported-yet 1:[for i in v:] for\n"));
    CHECK(body_reports("let h = x => x\nreturn k\n") == "unsupported-yet 1:[x => x] a lambda\n");
    CHECK(body_reports("let t = (k, k)\nreturn k\n") == "unsupported-yet 1:[(k, k)] a tuple\n");
    CHECK(body_reports("let t = k as vec3\nreturn k\n") == "unsupported-yet 1:[k as vec3] as\n");
    CHECK(body_reports("let t = k > 0.0 and k < 1.0\nreturn k\n").contains("a short-circuit operator\n"));
    CHECK(body_reports("let a = k\n") == "unsupported-yet 1:[let a = k] a body that does not end in return\n");
    CHECK(body_reports("fun g(x: float) -> float => x\nreturn k\n")
          == "unsupported-yet 1:[fun g(x: float) -> float => x] a declaration inside a function\n");
}

TEST("sgl check - the side tables name what an editor asks for")
{
    auto const checked = check_sources(read_prelude(), cc::string(pixel_edges)
                                                           + "@pixel fun ps(p: pin) -> target:\n"
                                                             "    let lit = p.color * 0.5\n"
                                                             "    return { color = float4(..lit, 1.0) }\n");
    CHECK(reports_of(checked) == "");
    auto const& m = checked.module;
    auto const& tables = m.files[1];

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
    CHECK(sgl::check::dump_entry_points(temporary.module)
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
        CHECK(x.from.file == 1);
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
