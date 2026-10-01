#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// A pixel stage that writes each spelling of the vector vocabulary a target has its own way of writing.
constexpr cc::string_view k_vectors = "struct pixel_input:\n"
                                      "    @position position: hpos4\n"
                                      "    uv: float2\n"
                                      "\n"
                                      "@pixel struct target:\n"
                                      "    color: float4\n"
                                      "\n"
                                      "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                      "    let v = p.uv\n"
                                      "    let w = fwidth(v)\n"
                                      "    let same = v == w\n"
                                      "    let apart = v != w\n"
                                      "    let each = equal(v, w)\n"
                                      "    let picked = select(v.x > 0.5, v, w)\n"
                                      "    let mixed = select(v > w, v, w)\n"
                                      "    let mid = float2(0.5)\n"
                                      "    let id = p.position.xy as int2\n"
                                      "    let masked = (id & 3) >> 1\n"
                                      "    let r = v % 0.5\n"
                                      "    let s = sign(id)\n"
                                      "    let tone = if v.x > 0.5 => sqrt(v.x) else v.y\n"
                                      "    let flag = if same or apart or any(each) => 1.0 else 0.0\n"
                                      "    return { color = float4(picked.x + mixed.y + mid.x + r.x + tone + flag, "
                                      "(masked.x + s.y) as float, 0.0, 1.0) }\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a vector's == and != are the whole value on every target, and equal is per component")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::wgsl, target::msl})
    {
        auto const text = text_of(k_vectors, t);
        CHECK(text.contains(" = all(v == w);\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains(" = any(v != w);\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains(" = v == w;\n")).dump("target", sgl::emit::to_string(t));
        CHECK(text.contains(" = fwidth(v);\n")).dump("target", sgl::emit::to_string(t));
    }
}

TEST("sgl emit - select takes its condition first in HLSL, and last in WGSL and MSL")
{
    auto const hlsl = text_of(k_vectors, target::hlsl_dx12);
    CHECK(hlsl.contains("const float2 picked = select(v.x > 0.5, v, w);\n"));
    CHECK(hlsl.contains("const float2 mixed = select(v > w, v, w);\n"));
    auto const wgsl = text_of(k_vectors, target::wgsl);
    CHECK(wgsl.contains("let picked: vec2f = select(w, v, v.x > 0.5);\n"));
    CHECK(wgsl.contains("let mixed: vec2f = select(w, v, v > w);\n"));
    // MSL's select takes a bool beside scalars alone, so a bool beside vectors is spread
    auto const msl = text_of(k_vectors, target::msl);
    CHECK(msl.contains("const float2 picked = select(w, v, bool2(v.x > 0.5));\n"));
    CHECK(msl.contains("const float2 mixed = select(w, v, v > w);\n"));
}

TEST("sgl emit - a one-value constructor and a scalar beside a vector are each target's own")
{
    auto const hlsl = text_of(k_vectors, target::hlsl_dx12);
    // HLSL's constructor wants every component, so one value is a cast
    CHECK(hlsl.contains("const float2 mid = (float2)0.5;\n"));
    CHECK(hlsl.contains("const int2 masked = (id & 3) >> 1;\n"));
    CHECK(hlsl.contains("const float2 r = v % 0.5;\n"));
    CHECK(hlsl.contains("const int2 s = sign(id);\n"));
    auto const wgsl = text_of(k_vectors, target::wgsl);
    CHECK(wgsl.contains(" = vec2f(0.5);\n"));
    // WGSL takes no scalar beside a vector for a bit operator or a shift
    CHECK(wgsl.contains("let masked: vec2i = (id & vec2i(3)) >> vec2u(vec2i(1));\n"));
    CHECK(wgsl.contains("let r: vec2f = v % 0.5;\n"));
    auto const msl = text_of(k_vectors, target::msl);
    CHECK(msl.contains(" = float2(0.5);\n"));
    // MSL's fmod takes no scalar beside a vector, and it has no sign of an integer
    CHECK(msl.contains("const float2 r = fmod(v, float2(0.5));\n"));
    CHECK(msl.contains("const int2 s = clamp(id, int2(-1), int2(1));\n"));
}

TEST("sgl emit - an if that is a value is a local each branch assigns, and only the taken branch runs")
{
    auto const hlsl = text_of(k_vectors, target::hlsl_dx12);
    CHECK(hlsl.contains("    float if_result;\n"
                        "    if (v.x > 0.5)\n"
                        "    {\n"
                        "        if_result = sqrt(v.x);\n"
                        "    }\n"
                        "    else\n"
                        "    {\n"
                        "        if_result = v.y;\n"
                        "    }\n"
                        "    const float tone = if_result;\n"));
    auto const wgsl = text_of(k_vectors, target::wgsl);
    CHECK(wgsl.contains("    var if_result: f32;\n"
                        "    if v.x > 0.5 {\n"
                        "        if_result = sqrt(v.x);\n"
                        "    } else {\n"
                        "        if_result = v.y;\n"
                        "    }\n"
                        "    let tone: f32 = if_result;\n"));
}

namespace
{
/// A function of the program that takes a texture, a sampler and an image, and one whose arrow body is a store.
constexpr cc::string_view k_resources
    = "binding post:\n"
      "    source: texture_2d[float4]\n"
      "    level: mut image_2d[.r32_float]\n"
      "    sampler bilinear:\n"
      "        filter = .linear\n"
      "\n"
      "fun fetch(t: texture_2d[float4], s: sampler, uv: float2) => t.sample(uv, s, level = 0.0)\n"
      "\n"
      "fun bump(img: mut image_2d[.r32_float], xy: int2, v: float) => img.store(xy, img.load(xy) + v)\n"
      "\n"
      "@compute(8, 8) fun main(@thread_id id: int3){post}:\n"
      "    let c = fetch(post.source, post.bilinear, (id.xy as float2) / 8.0)\n"
      "    bump(post.level, id.xy, c.x)\n";
} // namespace

TEST("sgl emit - a resource parameter is the member it was handed, written where the parameter is named")
{
    auto const hlsl = text_of(k_resources, target::hlsl_dx12);
    CHECK(hlsl.contains("post_source.SampleLevel(post_bilinear, "));
    CHECK(hlsl.contains("post_level[xy] = post_level[xy] + v;\n"));
    auto const wgsl = text_of(k_resources, target::wgsl);
    CHECK(wgsl.contains("textureSampleLevel(post_source, post_bilinear, "));
    CHECK(wgsl.contains("textureStore(post_level, xy, vec4f(textureLoad(post_level, xy).x + v, 0.0, 0.0, 0.0));\n"));
}
