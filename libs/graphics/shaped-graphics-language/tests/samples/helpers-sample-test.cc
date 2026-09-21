#include "../legalize/flat-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
cc::string read_helpers()
{
    return read_text(cc::string(SGL_SAMPLES_DIR) + "/helpers.sgl");
}

/// The pixel stage of the sample for `t`, from the line that starts the function.
cc::string pixel_function(sgl::emit::target t)
{
    auto const text = sgl::compile_to_text({.source = read_helpers(),
                                            .source_name = "helpers.sgl",
                                            .entry_point = "main_ps",
                                            .stage = stage::pixel,
                                            .target = t});
    if (!text.has_value())
        return text.error();
    auto at = text.value().text.find(t == sgl::emit::target::wgsl ? cc::string_view("@fragment")
                                                                  : cc::string_view("main_ps("));
    // the C-like targets put the result type in front of the name, on the same line
    while (at > 0 && text.value().text[at - 1] != '\n')
        --at;
    return at < 0
             ? text.value().text
             : cc::string(cc::string_view(text.value().text).subview({.start = at, .end = text.value().text.size()}));
}
} // namespace

TEST("sgl samples - helpers checks without a diagnostic, and every call is gone from its flat trees")
{
    auto const checked = check_sources(read_prelude(), read_helpers());
    CHECK(reports_of(checked) == "");
    auto const& m = checked.module;
    REQUIRE(m.entry_points.size() == 2);

    for (auto const& e : m.entry_points)
    {
        for (auto const& x : e.exprs)
            if (auto const* const call = x.node.try_as<flat_call>())
                CHECK(sgl::is_valid(call->intrinsic));
        auto const core = legalize(m, e);
        CHECK(!find_core_violation(core).has_value());
    }

    // `to_clip` reads the binding, and the entry point that calls it lists it
    auto const vs = dump_entry_point(m, m.entry_points[0]);
    CHECK(vs.contains("(entry vertex main_vs (v : surface_vertex) (uses constants) -> pixel_input\n"));
    CHECK(vs.contains("(block $to_clip\n"));
    CHECK(vs.contains("(leave $to_clip (call transform_position (binding constants view_projection : mat4) "));

    auto const ps = dump_entry_point(m, m.entry_points[1]);
    CHECK(ps.contains("(block $shade\n"));
    CHECK(ps.contains("(block $banded\n"));
    CHECK(ps.contains("(block $lambert\n"));
    CHECK(ps.contains("(block $rim\n"));
}

TEST("sgl samples - helpers is written for all four targets, and its WGSL reads like a shader")
{
    auto const source = read_helpers();
    for (auto const t : sgl::emit::all_targets())
    {
        CHECK(sgl::compile_to_text({.source = source, .entry_point = "main_vs", .stage = stage::vertex, .target = t})
                  .has_value());
        CHECK(sgl::compile_to_text({.source = source, .entry_point = "main_ps", .stage = stage::pixel, .target = t})
                  .has_value());
    }

    CHECK(pixel_function(sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn main_ps(p: pixel_input) -> target_ {\n"
             "    var shade_result: vec3f;\n"
             "    let color: vec3f = p.color;\n"
             "    let normal: vec3f = p.normal;\n"
             "    if length(normal) < 0.001 {\n"
             "        shade_result = color * 0.1;\n"
             "    } else {\n"
             "        let n: vec3f = normalize(normal);\n"
             "        var banded_result: f32;\n"
             "        var banded_left: bool = false;\n"
             "        loop {\n"
             "            let towards: vec3f = vec3f(0.45, 0.8, -0.4);\n"
             "            let light: f32 = saturate(dot(n, normalize(towards)));\n"
             "            if light <= 0.0 {\n"
             "                banded_result = 0.0;\n"
             "                break;\n"
             "            }\n"
             "            var edge: f32 = 0.0;\n"
             "            for (var i: i32 = 0; i < 4; i++) {\n"
             "                edge = edge + 0.25;\n"
             "                if light < edge {\n"
             "                    banded_result = edge;\n"
             "                    banded_left = true;\n"
             "                    break;\n"
             "                }\n"
             "            }\n"
             "            if banded_left {\n"
             "                break;\n"
             "            }\n"
             "            banded_result = 1.0;\n"
             "            break;\n"
             "        }\n"
             "        let light_1: f32 = banded_result;\n"
             "        var rim_result: f32;\n"
             "        let facing: f32 = abs(n.z);\n"
             "        if facing > 0.5 {\n"
             "            rim_result = 0.0;\n"
             "        } else {\n"
             "            rim_result = mix(0.35, 0.0, facing * 2.0);\n"
             "        }\n"
             "        shade_result = color * clamp(0.1 + light_1 + rim_result, 0.0, 1.0);\n"
             "    }\n"
             "    let lit: vec3f = shade_result;\n"
             "    return target_(vec4f(lit.x, lit.y, lit.z, 1.0));\n"
             "}\n");
    CHECK(pixel_function(sgl::emit::target::hlsl_dx12)
          == "target main_ps(pixel_input p)\n"
             "{\n"
             "    float3 shade_result;\n"
             "    const float3 color = p.color;\n"
             "    const float3 normal = p.normal;\n"
             "    if (length(normal) < 0.001)\n"
             "    {\n"
             "        shade_result = color * 0.1;\n"
             "    }\n"
             "    else\n"
             "    {\n"
             "        const float3 n = normalize(normal);\n"
             "        float banded_result;\n"
             "        bool banded_left = false;\n"
             "        do\n"
             "        {\n"
             "            const float3 towards = float3(0.45, 0.8, -0.4);\n"
             "            const float light = saturate(dot(n, normalize(towards)));\n"
             "            if (light <= 0.0)\n"
             "            {\n"
             "                banded_result = 0.0;\n"
             "                break;\n"
             "            }\n"
             "            float edge = 0.0;\n"
             "            for (int i = 0; i < 4; ++i)\n"
             "            {\n"
             "                edge = edge + 0.25;\n"
             "                if (light < edge)\n"
             "                {\n"
             "                    banded_result = edge;\n"
             "                    banded_left = true;\n"
             "                    break;\n"
             "                }\n"
             "            }\n"
             "            if (banded_left)\n"
             "            {\n"
             "                break;\n"
             "            }\n"
             "            banded_result = 1.0;\n"
             "        } while (false);\n"
             "        const float light_1 = banded_result;\n"
             "        float rim_result;\n"
             "        const float facing = abs(n.z);\n"
             "        if (facing > 0.5)\n"
             "        {\n"
             "            rim_result = 0.0;\n"
             "        }\n"
             "        else\n"
             "        {\n"
             "            rim_result = lerp(0.35, 0.0, facing * 2.0);\n"
             "        }\n"
             "        shade_result = color * clamp(0.1 + light_1 + rim_result, 0.0, 1.0);\n"
             "    }\n"
             "    const float3 lit = shade_result;\n"
             "    target result;\n"
             "    result.color = float4(lit.x, lit.y, lit.z, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl samples - the pipeline is total over helpers: every truncation checks, legalizes and is written or refused")
{
    auto const prelude = read_prelude();
    auto const source = read_helpers();
    // A prime stride cuts through every kind of token over the length of the file.
    for (auto length = isize(0); length < source.size(); length += 7)
    {
        auto const checked = check_sources(prelude, cc::string_view(source).subview({.offset = 0, .size = length}));
        auto const& m = checked.module;

        auto all_settled = true;
        for (auto const& s : m.symbols)
            all_settled = all_settled && (s.state == symbol_state::checked || s.state == symbol_state::failed);
        CHECK(all_settled);
        CHECK(m.entry_points.size() <= 2);
        CHECK(!dump(m).empty());

        for (auto const& e : m.entry_points)
        {
            auto const core = legalize(m, e);
            CHECK(!find_core_violation(core).has_value());
            (void)sgl::emit::emit_entry_point(m, core, sgl::emit::target::wgsl);
        }
    }
}
