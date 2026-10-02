#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// A compute stage that reads, writes and compound-assigns through swizzles of a local, of a buffer element and of a
/// struct of the program.
constexpr cc::string_view k_swizzles = "binding work:\n"
                                       "    points: mut buffer[float4]\n"
                                       "\n"
                                       "@swizzle struct rgb:\n"
                                       "    r: float\n"
                                       "    g: float\n"
                                       "    b: float\n"
                                       "\n"
                                       "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                       "    let i = id.x\n"
                                       "    let mut v = work.points[i]\n"
                                       "    v.zx = float2(1.0, 2.0)\n"
                                       "    v.yw += v.xz\n"
                                       "    work.points[i].wzy = (v + v).xyz\n"
                                       "    work.points[i].xy *= v.zw\n"
                                       "    let mut c = rgb(v.x, v.y, v.z)\n"
                                       "    c.bg = v.xy\n"
                                       "    work.points[i + 1] = float4(..c.gbr, c.rg.y)\n";

cc::string text_of(target t)
{
    auto const e = emit_source(k_swizzles, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a swizzle of a prelude vector is the target's own, read of any expression")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::wgsl, target::msl})
        CHECK(text_of(t).contains("(v + v).xyz")).dump("target", sgl::emit::to_string(t));
}

TEST("sgl emit - HLSL and MSL assign through a swizzle as it stands")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
    {
        auto const text = text_of(t);
        CHECK(text.contains("    v.zx = float2(1.0, 2.0);\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains("    v.yw = v.yw + v.xz;\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains("    work_points[i].wzy = (v + v).xyz;\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains("    work_points[i].xy = work_points[i].xy * v.zw;\n")).dump("target", sgl::emit::to_string(t));
    }
}

TEST("sgl emit - WGSL assigns one component at a time, from a local that holds the value")
{
    auto const wgsl = text_of(target::wgsl);
    CHECK(wgsl.contains("    let swizzled_1: vec2f = vec2f(1.0, 2.0);\n"
                        "    v.z = swizzled_1.x;\n"
                        "    v.x = swizzled_1.y;\n"));
    // a compound assignment reads the old value and the right side once, before any component is written
    CHECK(wgsl.contains("    let swizzled_2: vec2f = v.yw + v.xz;\n"
                        "    v.y = swizzled_2.x;\n"
                        "    v.w = swizzled_2.y;\n"));
    CHECK(wgsl.contains("    let swizzled_3: vec3f = (v + v).xyz;\n"
                        "    work_points[i].w = swizzled_3.x;\n"
                        "    work_points[i].z = swizzled_3.y;\n"
                        "    work_points[i].y = swizzled_3.z;\n"));
    CHECK(wgsl.contains("    let swizzled_4: vec2f = work_points[i].xy * v.zw;\n"
                        "    work_points[i].x = swizzled_4.x;\n"
                        "    work_points[i].y = swizzled_4.y;\n"));
}

TEST("sgl emit - a struct of the program swizzles by construction, and is written one field at a time everywhere")
{
    for (auto const t : {target::hlsl_dx12, target::wgsl, target::msl})
    {
        auto const text = text_of(t);
        CHECK(text.contains("    c.b = swizzled.x;\n"
                            "    c.g = swizzled.y;\n"))
            .dump("target", sgl::emit::to_string(t));
        // a splat of a swizzle is one field per letter, and a component of a swizzle is the field it names
        CHECK(text.contains("(c.g, c.b, c.r, c.g)")).dump("target", sgl::emit::to_string(t));
    }
}

TEST("sgl emit - a buffer element's index is evaluated once under a swizzle written one component at a time")
{
    auto const e = emit_source("binding work:\n"
                               "    points: mut buffer[float4]\n"
                               "    cursor: mut buffer[int]\n"
                               "\n"
                               "fun claim(){work} -> int:\n"
                               "    let at = work.cursor[0]\n"
                               "    work.cursor[0] = at + 1\n"
                               "    return at\n"
                               "\n"
                               "@compute(64) fun main(@thread_id id: int3){work}:\n"
                               "    work.points[claim()].yx += float2(1.0, 2.0)\n",
                               0, target::wgsl);
    CHECK(sgl::emit::dump_errors(e) == "");
    CHECK(e.text.contains("    let at_1: i32 = at;\n"
                          "    let swizzled: vec2f = work_points[at_1].yx + vec2f(1.0, 2.0);\n"
                          "    work_points[at_1].y = swizzled.x;\n"
                          "    work_points[at_1].x = swizzled.y;\n"));
}
