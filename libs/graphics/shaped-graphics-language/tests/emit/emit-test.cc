#include "emit-test-support.hh"

#include <shaped-graphics-language/emit/reserved_words.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// The edge structs most programs below share; `frame` and `pixel_input` are reserved nowhere.
constexpr cc::string_view k_edges = "@pixel struct frame:\n"
                                    "    color: float4\n"
                                    "\n"
                                    "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "    normal: vec3\n"
                                    "\n";

cc::string with_edges(cc::string_view rest)
{
    return cc::string(k_edges) + rest;
}

/// The text of entry point 0, which must exist.
cc::string text_of(cc::string_view user, target t)
{
    auto const e = emit_source(user, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// The errors of entry point 0, which must be the same for every target.
cc::string errors_of(cc::string_view user)
{
    auto const first = emit_source(user, 0, target::hlsl_dx12);
    CHECK(first.text == "");
    for (auto const t : sgl::emit::all_targets())
        CHECK(emit_source(user, 0, t) == first);
    return sgl::emit::dump_errors(first);
}
} // namespace

TEST("sgl emit - a local that is reserved in one target gets its underscore there and nowhere else")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let filter = normalize p.normal\n"
                                   "    let mul = saturate dot(filter, filter)\n"
                                   "    return {\n"
                                   "        color = float4(..filter, mul)\n"
                                   "    }\n");

    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    const float3 filter = normalize(p.normal);\n"));
    CHECK(hlsl.contains("    const float mul_ = saturate(dot(filter, filter));\n"));
    CHECK(hlsl.contains("    result.color = float4(filter, mul_);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    let filter_: vec3f = normalize(p.normal);\n"));
    CHECK(wgsl.contains("    let mul: f32 = saturate(dot(filter_, filter_));\n"));
    CHECK(wgsl.contains("    return frame(vec4f(filter_, mul));\n"));

    // `filter` is an enum of the metal namespace, and `mul` is nothing there
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    const float3 filter_ = normalize(p.normal);\n"));
    CHECK(msl.contains("    const float mul = saturate(dot(filter_, filter_));\n"));
    CHECK(msl.contains("    result.color = float4(filter_, mul);\n"));
}

TEST("sgl emit - a name that only MSL reserves is renamed in MSL and nowhere else")
{
    auto const source = with_edges("struct device:\n"
                                   "    fract: float\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   // written after it is built, so it stays a struct rather than its fields (LEGAL-61)
                                   "    let mut kernel = device(0.5)\n"
                                   "    if p.normal.x > 0.0 => kernel.fract = 0.25\n"
                                   "    return {\n"
                                   "        color = float4(kernel.fract, kernel.fract, kernel.fract, 1.0)\n"
                                   "    }\n");

    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct device_\n{\n    float fract_;\n};\n"));
    CHECK(msl.contains("    device_ kernel_;\n    kernel_.fract_ = 0.5;\n"));
    CHECK(msl.contains("float4(kernel_.fract_, kernel_.fract_, kernel_.fract_, 1.0);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct device {\n    fract: f32,\n}\n"));
    CHECK(wgsl.contains("    var kernel: device = device(0.5);\n"));
}

TEST("sgl emit - a reserved member is renamed, and its dx12 semantic still comes from the name as written")
{
    auto const source = cc::string_view("@inline binding constants:\n"
                                        "    matrix: mat4\n"
                                        "\n"
                                        "@vertex struct mesh_vertex:\n"
                                        "    point: pos3\n"
                                        "\n"
                                        "struct pixel_input:\n"
                                        "    @position position: hpos4\n"
                                        "\n"
                                        "@vertex fun main_vs(v: mesh_vertex){constants} -> pixel_input:\n"
                                        "    return {\n"
                                        "        position = constants.matrix * v.point\n"
                                        "    }\n");

    auto const hlsl = text_of(source, target::hlsl_vulkan);
    CHECK(hlsl.contains("    [[vk::location(0)]] float3 point_ : POINT;\n"));
    CHECK(hlsl.contains("    [[vk::offset(0)]] column_major float4x4 matrix_;\n"));
    CHECK(hlsl.contains("    result.position = mul(constants.matrix_, float4(v.point_, 1.0));\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    @location(0) point: vec3f,\n"));
    CHECK(wgsl.contains("    return pixel_input(constants.matrix * vec4f(v.point, 1.0));\n"));
}

TEST("sgl emit - mat4 of four columns is HLSL's constructor of four rows, transposed")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let c = float4(p.position.x, 0.0, 0.0, 1.0)\n"
                                   "    let m = mat4(c, c, c, c)\n"
                                   "    return { color = m * c }\n");

    CHECK(text_of(source, target::hlsl_dx12).contains("    const float4x4 m = transpose(float4x4(c, c, c, c));\n"));
    CHECK(text_of(source, target::wgsl).contains("    let m: mat4x4f = mat4x4f(c, c, c, c);\n"));
    CHECK(text_of(source, target::msl).contains("    const float4x4 m = float4x4(c, c, c, c);\n"));
}

TEST("sgl emit - a case on a bool local tests the local itself, with no copy and no `== true`")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let lit = p.normal.x > 0.5\n"
                                   "    let shade = case lit:\n"
                                   "        true => p.normal.y\n"
                                   "        false => p.normal.z\n"
                                   "    return { color = float4(shade) }\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(!wgsl.contains("case_value"));
    CHECK(!wgsl.contains("== true"));
    CHECK(wgsl.contains("    if lit {\n"));
}

TEST("sgl emit - a stage input nothing reads keeps its parameter, and has no line converting it")
{
    auto const source
        = cc::string_view("binding work:\n"
                          "    values: mut buffer[int]\n"
                          "@compute(8) fun main_cs(@thread_id id: int3, @local_thread_index li: int){work}:\n"
                          "    work.values[li] = 1\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("@builtin(global_invocation_id) id_in: vec3u"));
    CHECK(!wgsl.contains("let id: vec3i"));
    CHECK(wgsl.contains("let li: i32 = i32(li_in);"));
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(!hlsl.contains("const int3 id ="));
    CHECK(hlsl.contains("const int li = int(li_in);"));
}

TEST("sgl emit - an underscore that is taken already is minted past")
{
    auto const source = with_edges("struct target_:\n"
                                   "    x: float\n"
                                   "\n"
                                   "@pixel struct target:\n"
                                   "    color: float4\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                   "    let t = target_(0.5)\n"
                                   "    return {\n"
                                   "        color = float4(..p.normal, t.x)\n"
                                   "    }\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct target_ {\n    x: f32,\n}\n"));
    // the number follows the underscore directly, since `__` makes a name C++ and so MSL reserves
    CHECK(wgsl.contains("struct target_1 {\n    @location(0) color: vec4f,\n}\n"));
    CHECK(wgsl.contains("fn main_ps(p: pixel_input) -> target_1 {\n"));
}

TEST("sgl emit - a member named like its reserved type takes a number, never a second underscore")
{
    auto const source = with_edges("struct ray:\n"
                                   "    t: float\n"
                                   "\n"
                                   "struct shot:\n"
                                   "    ray: ray\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let s = shot(ray(p.normal.x))\n"
                                   "    return { color = float4(s.ray.t) }\n");
    // MSL reserves `ray`, so the type is `ray_`, and the member may not hide it (EMIT-138)
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    ray_ ray_1;\n"));
    CHECK(!msl.contains("__"));
}

TEST("sgl emit - an entry point a target reserves is renamed there, and the text says what it is called")
{
    // `main` is the canonical name for a shader, and MSL is the only target that forbids it.
    // So it is renamed there and kept everywhere else.
    // Whoever compiles the text asks for `entry_point`, never for the name they wrote.
    constexpr auto source = "@pixel fun main(p: pixel_input) -> frame:\n"
                            "    return {\n"
                            "        color = float4(..p.normal, 1.0)\n"
                            "    }\n";

    auto const msl = emit_source(with_edges(source), 0, target::msl);
    CHECK(sgl::emit::dump_errors(msl) == "");
    CHECK(msl.entry_point == "main_");
    CHECK(msl.text.contains("fragment frame main_(pixel_input p [[stage_in]])"));

    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const kept = emit_source(with_edges(source), 0, t);
        CHECK(sgl::emit::dump_errors(kept) == "");
        CHECK(kept.entry_point == "main");
    }

    // `filter` is WGSL's and MSL's, `mul` is HLSL's: each is renamed where it is taken and kept where it is free.
    auto const filtered = emit_source(with_edges("@pixel fun filter(p: pixel_input) -> frame:\n"
                                                 "    return {\n"
                                                 "        color = float4(..p.normal, 1.0)\n"
                                                 "    }\n"),
                                      0, target::wgsl);
    CHECK(filtered.entry_point == "filter_");

    auto const multiplied = emit_source(with_edges("@pixel fun mul(p: pixel_input) -> frame:\n"
                                                   "    return {\n"
                                                   "        color = float4(..p.normal, 1.0)\n"
                                                   "    }\n"),
                                        0, target::hlsl_dx12);
    CHECK(multiplied.entry_point == "mul_");

    // and the names the cube uses stay free everywhere, so nothing is renamed for it
    for (auto const t : sgl::emit::all_targets())
    {
        CHECK(!sgl::emit::is_reserved(t, "main_vs"));
        CHECK(!sgl::emit::is_reserved(t, "main_ps"));
    }
}

TEST("sgl emit - a group's plain member is a field of the constant buffer the group owns")
{
    // EMIT-83: the block is named after the binding, at slot 0, so a group without a buffer is one constant buffer.
    auto const source = with_edges("binding scene:\n"
                                   "    tint: float4\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input){scene} -> frame:\n"
                                   "    return {\n"
                                   "        color = scene.tint\n"
                                   "    }\n");
    CHECK(emit_source(source, 0, target::hlsl_dx12).errors.empty());
    CHECK(emit_source(source, 0, target::hlsl_vulkan).errors.empty());
    auto const wgsl = emit_source(source, 0, target::wgsl).text;
    CHECK(wgsl.contains("@group(0) @binding(0) var<uniform> scene: scene_data;"));
    CHECK(wgsl.contains("scene.tint")); // a member is read through the block

    // EMIT-89: MSL's group is an argument buffer, whose block is a pointer at slot 0, bound to a local of the block's name.
    auto const msl = emit_source(source, 0, target::msl);
    CHECK(sgl::emit::dump_errors(msl) == "");
    CHECK(msl.text.contains("struct scene_arguments\n{\n    constant scene_data* scene [[id(0)]];\n};\n"));
    CHECK(msl.text.contains("constant scene_arguments& scene_group [[buffer(0)]]"));
    CHECK(msl.text.contains("    constant auto& scene = *scene_group.scene;\n"));
    CHECK(msl.text.contains("scene.tint"));

    // A member with no place in a block has no address either, in a group as in an `@inline` binding.
    auto const e = emit_source(with_edges("binding scene:\n"
                                          "    lit: bool\n"
                                          "\n"
                                          "@pixel fun main_ps(p: pixel_input){scene} -> frame:\n"
                                          "    return {color = float4(1.0, 1.0, 1.0, 1.0)}\n"),
                               0, target::wgsl);
    REQUIRE(e.errors.size() == 1);
    CHECK(e.errors[0].kind == sgl::emit::error_kind::unsupported);
    CHECK(e.errors[0].detail
          == "a member of type 'bool' in a binding: 'scene.lit', whose bool has no layout; bool32 has one");
}

TEST("sgl emit - a group's constant buffer states every member's offset on vulkan, so no compiler flag lays it out")
{
    auto const source
        = with_edges("binding scene:\n"
                     "    exposure: float\n"
                     "    gamma: float\n"
                     "    jitter: float2\n"
                     "    tint: float3\n"
                     "\n"
                     "@pixel fun main_ps(p: pixel_input){scene} -> frame:\n"
                     "    return {\n"
                     "        color = float4(..scene.tint, scene.exposure + scene.gamma + scene.jitter.x)\n"
                     "    }\n");
    CHECK(emit_source(source, 0, target::hlsl_vulkan)
              .text.contains("struct scene_data\n"
                             "{\n"
                             "    [[vk::offset(0)]] float exposure;\n"
                             "    [[vk::offset(4)]] float gamma;\n"
                             "    [[vk::offset(8)]] float2 jitter;\n"
                             "    [[vk::offset(16)]] float3 tint;\n"
                             "};\n"));
    // dx12 gets none, as the push-constant block does not (EMIT-40): it packs a constant buffer this way by itself.
    CHECK(!emit_source(source, 0, target::hlsl_dx12).text.contains("vk::offset"));
}

TEST("sgl emit - a direction is transformed with w = 0, and WGSL parenthesizes the product it swizzles")
{
    auto const wgsl = text_of(with_edges("@inline binding look:\n"
                                         "    to_world: mat4\n"
                                         "\n"
                                         "@pixel fun main_ps(p: pixel_input){look} -> frame:\n"
                                         "    let n = look.to_world * p.normal\n"
                                         "    return {\n"
                                         "        color = float4(..n, 1.0)\n"
                                         "    }\n"),
                              target::wgsl);
    CHECK(wgsl.contains("    let n: vec3f = (look.to_world * vec4f(p.normal, 0.0)).xyz;\n"));
}

TEST("sgl emit - a float literal has a decimal point and reads back as the same value")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let a = saturate 3.0\n"
                                   "    let b = saturate 1e20\n"
                                   "    let c = saturate 2.5e-7\n"
                                   "    let d = saturate 0.1\n"
                                   "    let e = saturate 16777217.0\n"
                                   "    return {\n"
                                   "        color = float4(a, b, c, d * e)\n"
                                   "    }\n");
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("a = saturate(3.0);\n"));
    CHECK(hlsl.contains("b = saturate(1.0e+20);\n"));
    CHECK(hlsl.contains("c = saturate(2.5e-07);\n"));
    CHECK(hlsl.contains("d = saturate(0.1);\n"));
    CHECK(hlsl.contains("e = saturate(16777217.0);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("b: f32 = saturate(1.0e+20);\n"));
    CHECK(wgsl.contains("c: f32 = saturate(2.5e-07);\n"));
}

TEST("sgl emit - a hex literal stays hex, without its separators")
{
    auto const source = cc::string_view("binding work:\n"
                                        "    src: buffer[uint]\n"
                                        "    dst: mut buffer[uint]\n"
                                        "\n"
                                        "const mask = 0xff00u\n"
                                        "\n"
                                        "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                        "    let v = work.src[id.x]\n"
                                        "    let w = (v & 0xffff'fffdu) | mask\n"
                                        "    let s = (id.x & 0x2E74) - 16\n"
                                        "    work.dst[id.x] = w + (s as uint)\n");
    for (auto const t : {target::hlsl_dx12, target::wgsl, target::msl})
    {
        auto const text = text_of(source, t);
        // EMIT-158: a const keeps its radix as a literal does
        CHECK(text.contains("(v & 0xfffffffdu) | 0xff00u;\n"));
        CHECK(text.contains("(id.x & 0x2e74) - 16;\n"));
    }
}

TEST("sgl emit - a shift by a literal count needs no conversion and no mask")
{
    auto const source = cc::string_view("binding work:\n"
                                        "    src: buffer[int]\n"
                                        "    dst: mut buffer[int]\n"
                                        "\n"
                                        "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                        "    let v = work.src[id.x]\n"
                                        "    work.dst[id.x] = (v >> 12) + (v << 3) + (v >> id.x)\n");
    // CHK-270 holds a literal count to 0 to 31 already
    CHECK(text_of(source, target::wgsl).contains("(v >> 12u) + (v << 3u) + (v >> u32(id.x))"));
    CHECK(text_of(source, target::msl).contains("(v >> 12) + as_type<int>(as_type<uint>(v) << 3u) + (v >> (id.x & 31))"));
}

TEST("sgl emit - parentheses follow the tree, and a negative literal needs none")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let a = saturate 0.5\n"
                                   "    let b = (a + a) * a\n"
                                   "    let c = a + (a + b)\n"
                                   "    let d = a * b + c * -2.5\n"
                                   "    let e = a * (b * c)\n"
                                   "    return {\n"
                                   "        color = float4(b, c, d, e)\n"
                                   "    }\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    let b: f32 = (a + a) * a;\n"));
    CHECK(wgsl.contains("    let c: f32 = a + (a + b);\n"));
    CHECK(wgsl.contains("    let d: f32 = a * b + c * -2.5;\n"));
    CHECK(wgsl.contains("    let e: f32 = a * (b * c);\n"));
}

TEST("sgl emit - a struct of the program is declared before its first use, and built the way the target can")
{
    auto const source = with_edges("struct light:\n"
                                   "    towards: vec3\n"
                                   "    power: float\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   // written after it is built, so it stays a struct rather than its fields (LEGAL-61)
                                   "    let mut sun = light(normalize p.normal, 0.5)\n"
                                   "    if p.normal.x > 0.0 => sun.power = 1.0\n"
                                   "    return {\n"
                                   "        color = float4(..sun.towards, sun.power)\n"
                                   "    }\n");

    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("struct light\n{\n    float3 towards;\n    float power;\n};\n"));
    CHECK(hlsl.contains("    light sun;\n"
                        "    sun.towards = normalize(p.normal);\n"
                        "    sun.power = 0.5;\n"));
    // a splatted value that is no local was bound to a temporary by the check pass, and it reads like any `let`
    CHECK(hlsl.contains("    const float3 splat = sun.towards;\n"));
    CHECK(hlsl.contains("    result.color = float4(splat, sun.power);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct light {\n    towards: vec3f,\n    power: f32,\n}\n"));
    CHECK(wgsl.contains("    var sun: light = light(\n"
                        "        normalize(p.normal),\n"
                        "        0.5\n"
                        "    );\n"));
}

TEST("sgl emit - a struct read only field by field is its fields")
{
    // LEGAL-61: no struct is built, and each field is computed where it was
    auto const source = with_edges("struct light:\n"
                                   "    towards: vec3\n"
                                   "    power: float\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let sun = light(normalize p.normal, 0.5)\n"
                                   "    return {\n"
                                   "        color = float4(..sun.towards, sun.power)\n"
                                   "    }\n");
    for (auto const t : {target::hlsl_dx12, target::wgsl, target::msl})
    {
        auto const text = text_of(source, t);
        CHECK(!text.contains("light sun"));
        CHECK(!text.contains("sun: light"));
        CHECK(text.contains("normalize(p.normal)"));
    }
}

TEST("sgl emit - a vertex member whose name ends in a digit gets a semantic that does not")
{
    // HLSL reads `UV1` as the semantic `UV` at index 1, and dx12 refuses an input layout naming `UV1`.
    auto const source = cc::string("@vertex struct two_uvs:\n"
                                   "    uv0: float2\n"
                                   "    uv1: float2\n"
                                   "\n"
                                   "struct pixel_input:\n"
                                   "    @position position: hpos4\n"
                                   "\n"
                                   "@vertex fun main_vs(v: two_uvs) -> pixel_input:\n"
                                   "    return {\n"
                                   "        position = hpos4(v.uv0.x, v.uv1.y, 0.0, 1.0)\n"
                                   "    }\n");
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    float2 uv0 : UV0_;\n    float2 uv1 : UV1_;\n"));
}

TEST("sgl emit - a vertex member whose semantic another member took moves on")
{
    // `uv1_` would be `UV1_` too, and so would `UV1`, since a semantic ignores case.
    auto const source = cc::string("@vertex struct crowded:\n"
                                   "    uv1: float2\n"
                                   "    uv1_: float2\n"
                                   "    UV1: float2\n"
                                   "\n"
                                   "struct pixel_input:\n"
                                   "    @position position: hpos4\n"
                                   "\n"
                                   "@vertex fun main_vs(v: crowded) -> pixel_input:\n"
                                   "    return {\n"
                                   "        position = hpos4(v.uv1.x, v.uv1_.y, v.UV1.x, 1.0)\n"
                                   "    }\n");
    CHECK(text_of(source, target::hlsl_dx12)
              .contains("    float2 uv1 : UV1_;\n    float2 uv1_ : UV1__;\n    float2 UV1 : UV1___;\n"));
}

TEST("sgl emit - a pixel stage's depth and sample mask are outputs, not color targets")
{
    auto const source = cc::string("struct varyings:\n"
                                   "    @position position: hpos4\n"
                                   "\n"
                                   "@pixel struct shaded:\n"
                                   "    color: float4\n"
                                   "    @depth depth: float\n"
                                   "    @sample_mask covered: uint\n"
                                   "\n"
                                   "@pixel fun shade_ps(p: varyings) -> shaded:\n"
                                   "    return { color = float4(1.0, 1.0, 1.0, 1.0), depth = 0.5, covered = 0x1 }\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::wgsl})
    {
        auto const e = emit_source(source, 0, t);
        CHECK(sgl::emit::dump_errors(e) == "");
        CHECK(e.color_targets == 1);
        CHECK(e.target_struct == "shaded");
    }
    CHECK(!text_of(source, target::hlsl_dx12).contains("centroid"));
}

TEST("sgl emit - a pixel stage writing a conservative depth takes its position at the centroid on dx12")
{
    // DXIL refuses SV_DepthGreaterEqual from a pixel stage whose SV_Position is interpolated at the pixel centre.
    auto const source = cc::string("struct varyings:\n"
                                   "    @position position: hpos4\n"
                                   "\n"
                                   "@pixel struct pushed:\n"
                                   "    color: float4\n"
                                   "    @depth(.greater_equal) depth: float\n"
                                   "\n"
                                   "@pixel fun push_ps(p: varyings) -> pushed:\n"
                                   "    return { color = float4(1.0, 1.0, 1.0, 1.0), depth = p.position.z + 0.5 }\n");
    CHECK(text_of(source, target::hlsl_dx12).contains("    noperspective centroid float4 position : SV_Position;\n"));
    CHECK(text_of(source, target::hlsl_vulkan).contains("    float4 position : SV_Position;\n"));
}

TEST("sgl emit - a member whose dx12 semantic would be a system value is an error")
{
    CHECK(errors_of("@vertex struct mesh_vertex:\n"
                    "    sv_position: pos3\n"
                    "\n"
                    "struct pixel_input:\n"
                    "    @position position: hpos4\n"
                    "\n"
                    "@inline binding constants:\n"
                    "    view_projection: mat4\n"
                    "\n"
                    "@vertex fun main_vs(v: mesh_vertex){constants} -> pixel_input:\n"
                    "    return {\n"
                    "        position = constants.view_projection * v.sv_position\n"
                    "    }\n")
          == "system-value-semantic 'mesh_vertex.sv_position'\n");
}

TEST("sgl emit - emit is total: a module with errors and a position out of range are errors in the result")
{
    auto const broken = check_sources(read_prelude(), with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                                                 "    return {\n"
                                                                 "        color = nothing\n"
                                                                 "    }\n"));
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(broken.module, 0, target::wgsl)) == "module-has-errors 1 errors\n");

    auto const cube = check_sources(read_prelude(), read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(cube.module, 2, target::wgsl)) == "unknown-entry-point 2 of 2\n");
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(cube.module, -1, target::hlsl_dx12)) == "unknown-entry-point -1 of 2\n");
}

TEST("sgl emit - the reserved words differ by target and hold no word twice")
{
    CHECK(sgl::emit::is_reserved(target::wgsl, "target"));
    CHECK(sgl::emit::is_reserved(target::wgsl, "filter"));
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "target"));
    CHECK(sgl::emit::is_reserved(target::hlsl_vulkan, "float4x4"));
    CHECK(sgl::emit::is_reserved(target::msl, "main"));
    CHECK(sgl::emit::is_reserved(target::msl, "fragment"));
    CHECK(sgl::emit::is_reserved(target::msl, "packed_float3"));
    CHECK(!sgl::emit::is_reserved(target::msl, "target"));
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "main"));
    // what the hand-written cube shaders call their locals stays free
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "lit"));
    CHECK(!sgl::emit::is_reserved(target::wgsl, "input"));
    CHECK(!sgl::emit::is_reserved(target::msl, "lit"));

    for (auto const t : sgl::emit::all_targets())
    {
        auto const words = sgl::emit::reserved_words(t);
        auto duplicates = cc::string();
        for (auto i = isize(0); i < words.size(); ++i)
            for (auto j = i + 1; j < words.size(); ++j)
                if (words[i] == words[j])
                    duplicates.appendf("{} ", words[i]);
        CHECK(duplicates == "");
    }
}

TEST("sgl emit - a local named after a function is a local of its own in the text")
{
    // CHK-105: every module-level name is taken before a local is minted
    auto const wgsl = text_of(with_edges("fun helper(k: float) -> float:\n"
                                         "    return k * 2.0\n"
                                         "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                         "    let helper = helper p.normal.x\n"
                                         "    return { color = float4(helper, helper, helper, 1.0) }\n"),
                              target::wgsl);
    CHECK(wgsl.contains("let helper_1: f32"));
}

TEST("sgl emit - a function of the user file with a prelude function's parameter types is the one called")
{
    // CHK-192: the call is the inlined body of the user file's `dot`, not the builtin
    auto const wgsl = text_of(with_edges("fun dot(a: vec3, b: vec3) -> float => 7.0\n"
                                         "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                         "    let d = dot(p.normal, p.normal)\n"
                                         "    return { color = float4(d, d, d, 1.0) }\n"),
                              target::wgsl);
    CHECK(wgsl.contains("7.0"));
    CHECK(!wgsl.contains("dot("));
}

TEST("sgl emit - a struct that shadows one of the prelude is written under a name of its own")
{
    // the prelude's `light` reaches the entry point through `lit`, and the user file's through `dim`
    auto sources = cc::vector<cc::string_view>();
    for (auto const& p : sgl::prelude_files())
        sources.push_back(p.source);
    sources.push_back("struct light:\n"
                      "    a: float\n"
                      "fun lit(k: float) -> light:\n"
                      "    return { a = k }\n");
    auto const user = with_edges("struct light:\n"
                                 "    b: float\n"
                                 "fun dim(k: float) -> light:\n"
                                 "    return { b = k * 0.5 }\n"
                                 "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                 "    let l = lit p.normal.x\n"
                                 "    let m = dim l.a\n"
                                 "    return { color = float4(l.a, m.b, 0.0, 1.0) }\n");
    sources.push_back(user);
    auto const checked = check_files(sources);
    REQUIRE(reports_of(checked) == "");

    for (auto const t : sgl::emit::all_targets())
    {
        auto const e = sgl::emit::emit(checked.module, 0, t);
        CHECK(sgl::emit::dump_errors(e) == "");
        CHECK(e.text.contains("struct light"));
        CHECK(e.text.contains("struct light_1"));
    }
}
