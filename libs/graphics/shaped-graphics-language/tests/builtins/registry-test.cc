#include "../check/source-flat-test-support.hh"

#include <shaped-graphics-language/builtins/register.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;
namespace builtins = sgl::builtins;

namespace
{
/// A sawtooth of period 1, which the library does not have: what this file adds to a registry of its own.
void sawtooth(cc::span<scalar const> in, cc::vector<scalar>& out)
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

    // a record is one overload: the name, the parameter types and the named-only names, read from the signature's own text
    cc::string_view const positional[] = {"", "", ""};
    cc::string_view const two_positional[] = {"", ""};
    cc::string_view const on_vec3[] = {"vec3", "vec3"};
    cc::string_view const on_float3[] = {"float3", "float3"};
    auto const dot_vec3 = r.find_function("dot", on_vec3, two_positional);
    auto const dot_float3 = r.find_function("dot", on_float3, two_positional);
    REQUIRE(sgl::is_valid(dot_vec3));
    REQUIRE(sgl::is_valid(dot_float3));
    CHECK(dot_vec3 != dot_float3);
    CHECK(r.at(dot_vec3).result == r.find_type("float"));
    cc::string_view const mixed[] = {"vec3", "float3"};
    CHECK(!sgl::is_valid(r.find_function("dot", mixed, two_positional)));

    // `.level` and `.bias` are one type, and two overloads
    cc::string_view const on_sample[] = {"texture_2d[float4]", "float2", "sampler", "float"};
    cc::string_view const at_level[] = {"", "", "", "level"};
    cc::string_view const with_bias[] = {"", "", "", "bias"};
    auto const level = r.find_function("sample", on_sample, at_level);
    auto const bias = r.find_function("sample", on_sample, with_bias);
    REQUIRE(sgl::is_valid(level));
    REQUIRE(sgl::is_valid(bias));
    CHECK(level != bias);
    cc::string_view const all_positional[] = {"", "", "", ""};
    CHECK(!sgl::is_valid(r.find_function("sample", on_sample, all_positional)));

    // no two records are the same overload, or a declaration could not say which one it stands for
    for (auto i = isize(0); i < r.functions.size(); ++i)
    {
        auto parameters = cc::vector<cc::string_view>();
        auto named_only = cc::vector<cc::string_view>();
        for (auto const& p : r.functions[i].parameters)
            parameters.push_back(p);
        for (auto const& n : r.functions[i].named_only)
            named_only.push_back(n);
        CHECK(r.find_function(r.functions[i].name, parameters, named_only) == sgl::builtin_id(i));
    }

    // the one name a target spells differently so far
    cc::string_view const on_mix[] = {"float3", "float3", "float"};
    auto const mix = r.find_function("mix", on_mix, positional);
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
        // an operator, that is: `distance(a, b)` of two positions is a length, and means something
        if (is_pair("pos3", "pos3") && f.signature.contains("@operator"))
            CHECK(f.result == vec3);
        CHECK(!(is_pair("vec3", "vec3") && f.name.starts_with("multiply")));
    }
}

TEST("sgl builtins - one record is all a new builtin takes: checked, run and written for every target")
{
    // A registry of the test's own: everything the library has, and `sawtooth`.
    auto r = builtins::registry();
    builtins::register_builtins(r);
    r.add(builtins::function_record{
        .signature = "@pure fun sawtooth(x: float) -> float",
        .doc = "/// What is left of `x` behind the point.",
        .evaluate = sawtooth,
        .write = {.hlsl = "frac", .wgsl = "fract", .msl = "fract"},
    });
    r.finalize();
    CHECK(r.prelude_text().ends_with("/// What is left of `x` behind the point.\n@builtin @pure fun sawtooth(x: float) "
                                     "-> "
                                     "float\n"));

    auto const prelude = sgl::parse(r.prelude_text());
    auto const prelude_ast = sgl::ast::build(prelude);
    auto const user = sgl::parse(cc::string(frag_edges)
                                 + "@pixel fun main_ps(p: frag) -> target:\n"
                                   "    let x = sawtooth(p.a * 10.0)\n"
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

TEST("sgl builtins - a texture method's MSL, which no group reaches until MSL takes one")
{
    auto const& r = builtins::default_registry();
    // the call a record writes, with its arguments already written as the names of their parameters
    auto const msl = [&](cc::string_view name, cc::span<cc::string_view const> types,
                         cc::span<cc::string_view const> named_only, cc::span<cc::string_view const> arguments)
    {
        auto const id = r.find_function(name, types, named_only);
        REQUIRE(sgl::is_valid(id));
        auto written = cc::vector<builtins::written>();
        for (auto const a : arguments)
            written.push_back({.text = cc::string(a)});
        auto const& record = r.at(id);
        return record.write
            .custom({.target = builtins::language::msl, .arguments = written, .builtins = r, .data = record.write.data})
            .text;
    };

    cc::string_view const bias_types[] = {"texture_2d_array[float4]", "float2", "sampler", "int", "float"};
    cc::string_view const bias_names[] = {"", "", "", "layer", "bias"};
    cc::string_view const bias_args[] = {"t", "uv", "s", "2", "0.5"};
    CHECK(msl("sample", bias_types, bias_names, bias_args) == "t.sample(s, uv, uint(2), bias(0.5))");

    // a gather names its component, and takes an offset before it on every shape but a cube
    cc::string_view const gather_types[] = {"texture_2d[float4]", "float2", "sampler", "texel_component", "int2"};
    cc::string_view const gather_names[] = {"", "", "", "component", "offset"};
    cc::string_view const gather_args[] = {"t", "uv", "s", "3", "int2(1, 0)"};
    CHECK(msl("gather", gather_types, gather_names, gather_args) == "t.gather(s, uv, int2(1, 0), component::w)");
    cc::string_view const cube_types[] = {"texture_cube[float4]", "float3", "sampler", "texel_component"};
    cc::string_view const cube_names[] = {"", "", "", "component"};
    cc::string_view const cube_args[] = {"t", "d", "s", "1"};
    CHECK(msl("gather", cube_types, cube_names, cube_args) == "t.gather(s, d, component::y)");

    cc::string_view const compare_types[] = {"texture_2d_depth", "float2", "comparison_sampler", "float", "float"};
    cc::string_view const compare_names[] = {"", "", "", "reference", "level"};
    cc::string_view const compare_args[] = {"t", "uv", "c", "0.5", "0.0"};
    CHECK(msl("sample_compare", compare_types, compare_names, compare_args) == "t.sample_compare(c, uv, 0.5, level(0))");

    // a load of three channels reads four, and keeps its own
    cc::string_view const load_types[] = {"texture_2d[float3]", "int2", "int"};
    cc::string_view const load_names[] = {"", "", ""};
    cc::string_view const load_args[] = {"t", "xy", "1"};
    CHECK(msl("load", load_types, load_names, load_args) == "t.read(uint2(xy), uint(1)).xyz");

    // a store pads its value to four channels of its own kind
    cc::string_view const store_types[] = {"out image_2d_array[uint]", "int2", "uint", "int"};
    cc::string_view const store_names[] = {"", "", "", "layer"};
    cc::string_view const store_args[] = {"i", "xy", "v", "3"};
    CHECK(msl("store", store_types, store_names, store_args) == "i.write(uint4(v, 0u, 0u, 0u), uint2(xy), uint(3))");

    cc::string_view const size_types[] = {"texture_3d", "int"};
    cc::string_view const size_names[] = {"", ""};
    cc::string_view const size_args[] = {"t", "2"};
    CHECK(msl("size", size_types, size_names, size_args) == "int3(t.get_width(2), t.get_height(2), t.get_depth(2))");
}
