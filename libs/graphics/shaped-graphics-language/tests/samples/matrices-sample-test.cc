#include "../legalize/flat-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
cc::string read_matrices()
{
    return read_text(cc::string(SGL_SAMPLES_DIR) + "/matrices.sgl");
}

/// The function of `entry` for `t`, from the line that starts it.
cc::string function_text(cc::string_view entry, sgl::emit::target t)
{
    auto const text = sgl::compile_to_text(
        {.source = read_matrices(), .source_name = "matrices.sgl", .entry_point = entry, .target = t});
    if (!text.has_value())
        return text.error();
    auto at = text.value().find(cc::string(entry) + "(");
    while (at > 0 && text.value()[at - 1] != '\n')
        --at;
    return at < 0 ? text.value()
                  : cc::string(cc::string_view(text.value()).subview({.start = at, .end = text.value().size()}));
}

/// Column-major, so a translation stands in leaves 12 to 14.
value translation(checked_module const& m, f32 x, f32 y, f32 z)
{
    auto result = zero_value(m, flat_builder{.m = m}.type_named("mat4"));
    for (auto i = 0; i < 4; ++i)
        result.leaves[i * 5] = scalar::of(1.0f);
    result.leaves[12] = scalar::of(x);
    result.leaves[13] = scalar::of(y);
    result.leaves[14] = scalar::of(z);
    return result;
}
} // namespace

TEST("sgl samples - matrices checks: the helper's result is inferred, and its binding reaches the entry point")
{
    auto const checked = check_sources(read_prelude(), read_matrices());
    CHECK(reports_of(checked) == "");
    auto const& m = checked.module;
    REQUIRE(m.entry_points.size() == 2);

    auto const symbols = dump(m);
    CHECK(symbols.contains("(fun make_mvp (model : mat4) (uses frame) -> mat4)\n"));
    CHECK(symbols.contains("(fun multiply_mat4 builtin pure operator:* (m : mat4) (b : mat4) -> mat4)\n"));

    auto const vs = dump_entry_point(m, m.entry_points[0]);
    CHECK(vs.contains("(block $make_mvp\n"));
    CHECK(vs.contains("(call multiply_mat4 (call multiply_mat4 (binding frame proj : mat4) (binding frame view : mat4) "
                      ": mat4) "));

    // a call whose value is dropped is a statement of its own, in the tree the check pass writes
    auto const ps = dump_entry_point(m, m.entry_points[1]);
    CHECK(ps.contains("  (eval (call saturate (member (local n : vec3) x : float) : float))\n"));
}

TEST("sgl samples - matrices runs: proj * view * model is one matrix, applied to the position")
{
    auto const checked = check_sources(read_prelude(), read_matrices());
    REQUIRE(reports_of(checked) == "");
    auto const& m = checked.module;
    auto const& vs = m.entry_points[0];

    // three translations multiply to their sum, in any order, which is what makes the expected value easy to read
    value const parts[]
        = {translation(m, 1.0f, 0.0f, 0.0f), translation(m, 0.0f, 2.0f, 0.0f), translation(m, 0.0f, 0.0f, 4.0f)};
    auto frame = value();
    for (auto const& part : parts)
        frame.leaves.push_back_range(part.leaves);
    auto inputs = run_inputs{.parameter = zero_value(m, vs.input)};
    inputs.parameter.leaves[0] = scalar::of(10.0f);
    inputs.parameter.leaves[1] = scalar::of(20.0f);
    inputs.parameter.leaves[2] = scalar::of(30.0f);
    // the normal, which takes the model matrix without its translation
    inputs.parameter.leaves[3] = scalar::of(0.0f);
    inputs.parameter.leaves[4] = scalar::of(1.0f);
    inputs.parameter.leaves[5] = scalar::of(0.0f);
    inputs.bindings.push_back(frame);

    auto const structured = interpret(m, vs, inputs);
    CHECK(dump(structured) == "ok 11 22 34 1 0 1 0");
    CHECK(interpret(m, legalize(m, vs), inputs) == structured);
}

TEST("sgl samples - matrices is written for all four targets")
{
    CHECK(function_text("main_vs", sgl::emit::target::hlsl_dx12)
          == "pixel_input main_vs(mesh_vertex v)\n"
             "{\n"
             "    const float4x4 model = frame.model;\n"
             "    const float4x4 mvp = mul(mul(frame.proj, frame.view), model);\n"
             "    pixel_input result;\n"
             "    result.position = mul(mvp, float4(v.position, 1.0));\n"
             "    result.normal = mul(frame.model, float4(v.normal, 0.0)).xyz;\n"
             "    return result;\n"
             "}\n");
    CHECK(function_text("main_vs", sgl::emit::target::wgsl)
          == "fn main_vs(v: mesh_vertex) -> pixel_input {\n"
             "    let model: mat4x4f = frame.model;\n"
             "    let mvp: mat4x4f = frame.proj * frame.view * model;\n"
             "    return pixel_input(\n"
             "        mvp * vec4f(v.position, 1.0),\n"
             "        (frame.model * vec4f(v.normal, 0.0)).xyz\n"
             "    );\n"
             "}\n");

    // the dropped value, as each language says it
    CHECK(function_text("main_ps", sgl::emit::target::hlsl_vulkan).contains("    saturate(n.x);\n"));
    CHECK(function_text("main_ps", sgl::emit::target::wgsl).contains("    _ = saturate(n.x);\n"));
    CHECK(function_text("main_ps", sgl::emit::target::msl).contains("    (void)(saturate(n.x));\n"));
}
