#include "../check/source-flat-test-support.hh"

#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;
namespace builtins = sgl::builtins;

namespace
{
/// The fractional part, which the library does not have: what this file adds to a registry of its own.
void fract(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    auto const x = in[0].as_float();
    out.push_back(scalar::of(x - f32(i32(x))));
}
} // namespace

TEST("sgl builtins - the registry generates a prelude that parses, and reads every record back from it")
{
    auto const& r = builtins::default_registry();
    auto const file = sgl::parse(r.prelude_text());
    auto const ast = sgl::ast::build(file);
    CHECK(file.diagnostics.empty());
    CHECK(ast.diagnostics.empty());
    CHECK(isize(ast.declarations.count) == r.types.size() + r.functions.size());
    CHECK(r.prelude_text().starts_with("// GENERATED FILE - DO NOT EDIT.\n"));

    // Deterministic: registration is one function calling the topics in a fixed order.
    CHECK(builtins::make_registry().prelude_text() == r.prelude_text());

    auto const float3 = r.find_type("float3");
    auto const vec3 = r.find_type("vec3");
    REQUIRE(sgl::is_valid(float3));
    REQUIRE(sgl::is_valid(vec3));
    CHECK(!sgl::is_valid(r.find_type("quaternion")));
    CHECK(r.at(float3).spelled_in(builtins::language::wgsl) == "vec3f");
    CHECK(r.at(float3).leaf_count == 3);

    // a record is one overload: the name and the parameter types, read from the signature's own text
    cc::string_view const on_vec3[] = {"vec3", "vec3"};
    cc::string_view const on_float3[] = {"float3", "float3"};
    auto const dot_vec3 = r.find_function("dot", on_vec3);
    auto const dot_float3 = r.find_function("dot", on_float3);
    REQUIRE(sgl::is_valid(dot_vec3));
    REQUIRE(sgl::is_valid(dot_float3));
    CHECK(dot_vec3 != dot_float3);
    CHECK(r.at(dot_vec3).result == r.find_type("float"));
    cc::string_view const mixed[] = {"vec3", "float3"};
    CHECK(!sgl::is_valid(r.find_function("dot", mixed)));

    // no two records are the same overload, or a declaration could not say which one it stands for
    for (auto i = isize(0); i < r.functions.size(); ++i)
    {
        auto parameters = cc::vector<cc::string_view>();
        for (auto const& p : r.functions[i].parameters)
            parameters.push_back(p);
        CHECK(r.find_function(r.functions[i].name, parameters) == sgl::builtin_id(i));
    }

    // the one name a target spells differently so far
    cc::string_view const on_mix[] = {"float3", "float3", "float"};
    auto const mix = r.find_function("mix", on_mix);
    REQUIRE(sgl::is_valid(mix));
    CHECK(r.at(mix).called_in(builtins::language::hlsl) == "lerp");
    CHECK(r.at(mix).called_in(builtins::language::msl) == "mix");
}

TEST("sgl builtins - what means nothing is not there: a position plus a position, a direction times a direction")
{
    auto const& r = builtins::default_registry();
    auto const vec3 = r.find_type("vec3");
    for (auto const& f : r.functions)
    {
        auto const is_pair = [&](cc::string_view a, cc::string_view b)
        { return f.parameters.size() == 2 && f.parameters[0] == a && f.parameters[1] == b; };
        if (is_pair("pos3", "pos3"))
            CHECK(f.result == vec3);
        CHECK(!(is_pair("vec3", "vec3") && f.name.starts_with("multiply")));
    }
}

TEST("sgl builtins - one record is all a new builtin takes: checked, run and written for every target")
{
    // A registry of the test's own: everything the library has, and `fract`.
    auto r = builtins::registry();
    builtins::register_builtins(r);
    r.add(builtins::function_record{
        .signature = "@pure fun fract(x: float) -> float",
        .doc = "/// What is left of `x` behind the point.",
        .evaluate = fract,
        .write = {.hlsl = "frac"},
    });
    r.finalize();
    CHECK(r.prelude_text().ends_with("/// What is left of `x` behind the point.\n@builtin @pure fun fract(x: float) -> "
                                     "float\n"));

    auto const prelude = sgl::parse(r.prelude_text());
    auto const prelude_ast = sgl::ast::build(prelude);
    auto const user = sgl::parse(cc::string(frag_edges)
                                 + "@pixel fun main_ps(p: frag) -> target:\n"
                                   "    let x = fract(p.a * 10.0)\n"
                                   "    return {\n"
                                   "        color = float4(x, x, x, 1.0)\n"
                                   "    }\n");
    auto const user_ast = sgl::ast::build(user);
    module_file const files[] = {{.file = prelude, .ast = prelude_ast}};
    auto const m = check(files, {.file = user, .ast = user_ast}, r);
    CHECK(dump_diagnostics(m) == "");
    REQUIRE(m.entry_points.size() == 1);

    // the library's own registry knows no such builtin, and says so
    CHECK(dump_diagnostics(check(files, {.file = user, .ast = user_ast})).contains("unknown-builtin"));

    auto inputs = run_inputs{.parameter = zero_value(m, m.entry_points[0].input)};
    inputs.parameter.leaves[0] = scalar::of(0.125f);
    CHECK(dump(interpret(m, m.entry_points[0], inputs)) == "ok 0.25 0.25 0.25 1");

    auto const hlsl = sgl::emit::emit(m, 0, sgl::emit::target::hlsl_dx12);
    auto const wgsl = sgl::emit::emit(m, 0, sgl::emit::target::wgsl);
    REQUIRE(hlsl.has_text());
    REQUIRE(wgsl.has_text());
    CHECK(hlsl.text.contains("    const float x = frac(p.a * 10.0);\n"));
    CHECK(wgsl.text.contains("    let x: f32 = fract(p.a * 10.0);\n"));
}

TEST("sgl builtins - a local may not hide the function a builtin is written as, in the target that writes it so")
{
    // `lerp` is what HLSL calls `mix`, so a local of that name is renamed there and nowhere else.
    auto const checked = check_sources(read_prelude(), cc::string(frag_edges)
                                                           + "@pixel fun main_ps(p: frag) -> target:\n"
                                                             "    let lerp = mix(p.a, p.b, 0.5)\n"
                                                             "    return {\n"
                                                             "        color = float4(lerp, lerp, lerp, 1.0)\n"
                                                             "    }\n");
    REQUIRE(reports_of(checked) == "");
    auto const hlsl = sgl::emit::emit(checked.module, 0, sgl::emit::target::hlsl_dx12);
    auto const wgsl = sgl::emit::emit(checked.module, 0, sgl::emit::target::wgsl);
    REQUIRE(hlsl.has_text());
    REQUIRE(wgsl.has_text());
    CHECK(hlsl.text.contains("const float lerp_ = lerp(p.a, p.b, 0.5);"));
    CHECK(wgsl.text.contains("let lerp: f32 = mix(p.a, p.b, 0.5);"));
}
