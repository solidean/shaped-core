#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// One of each resource a post-processing pass has: sample a texture, write one image, accumulate into another.
constexpr cc::string_view k_blur = "binding post:\n"
                                   "    texel_size: float2\n"
                                   "    src: texture_2d[float4]\n"
                                   "    dst: out image_2d[.rgba8_unorm]\n"
                                   "    acc: mut image_2d[.r32_float]\n"
                                   "    sampler bilinear:\n"
                                   "        filter = .linear\n"
                                   "        address = .clamp_edge\n"
                                   "\n"
                                   "@compute(8, 8) fun blur(@thread_id id: int3){post}:\n"
                                   "    let xy = int2(id.x, id.y)\n"
                                   "    let uv = ((xy as float2) + float2(0.5, 0.5)) * post.texel_size\n"
                                   "    let c = DEBUG_sample_level(post.src, uv, 0.0, post.bilinear)\n"
                                   "    DEBUG_store(post.dst, xy, c)\n"
                                   "    DEBUG_store(post.acc, xy, DEBUG_load(post.acc, xy) + c.x)\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - textures, images and a static sampler are resources of the group, in declaration order")
{
    CHECK(text_of(k_blur, target::wgsl)
          == "// SGL compute entry point 'blur', written as WGSL.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct post_data {\n"
             "    texel_size: vec2f,\n"
             "}\n"
             "\n"
             "@group(0) @binding(0) var<uniform> post: post_data;\n"
             "@group(0) @binding(1) var post_src: texture_2d<f32>;\n"
             "@group(0) @binding(2) var post_dst: texture_storage_2d<rgba8unorm, write>;\n"
             "@group(0) @binding(3) var post_acc: texture_storage_2d<r32float, read_write>;\n"
             "@group(0) @binding(4) var post_bilinear: sampler;\n"
             "\n"
             "@compute @workgroup_size(8, 8, 1)\n"
             "fn blur(@builtin(global_invocation_id) id_in: vec3u) {\n"
             "    let id: vec3i = vec3i(id_in);\n"
             "    let xy: vec2i = vec2i(id.x, id.y);\n"
             "    let uv: vec2f = (vec2f(xy) + vec2f(0.5, 0.5)) * post.texel_size;\n"
             "    let c: vec4f = textureSampleLevel(post_src, post_bilinear, uv, 0.0);\n"
             "    textureStore(post_dst, xy, vec4f(c));\n"
             "    textureStore(post_acc, xy, vec4f(textureLoad(post_acc, xy).x + c.x, 0.0, 0.0, 0.0));\n"
             "}\n");

    auto const vulkan = text_of(k_blur, target::hlsl_vulkan);
    CHECK(vulkan.contains("};\n"
                          "\n"
                          "[[vk::binding(0, 0)]] ConstantBuffer<post_data> post;\n"
                          "[[vk::binding(1, 0)]] Texture2D<float4> post_src;\n"
                          "[[vk::binding(2, 0)]] [[vk::image_format(\"rgba8\")]] RWTexture2D<float4> post_dst;\n"
                          "[[vk::binding(3, 0)]] [[vk::image_format(\"r32f\")]] RWTexture2D<float> post_acc;\n"
                          "[[vk::binding(4, 0)]] SamplerState post_bilinear;\n"
                          "\n"));
    CHECK(vulkan.contains("    const float4 c = post_src.SampleLevel(post_bilinear, uv, "
                          "0.0);\n"
                          "    post_dst[xy] = c;\n"
                          "    post_acc[xy] = post_acc[xy] + c.x;\n"));

    // dx12 addresses the same slots as registers of the group's space, and leaves an image's format to the view.
    auto const dx12 = text_of(k_blur, target::hlsl_dx12);
    CHECK(dx12.contains("};\n"
                        "\n"
                        "ConstantBuffer<post_data> post : register(b0, space0);\n"
                        "Texture2D<float4> post_src : register(t1, space0);\n"
                        "RWTexture2D<float4> post_dst : register(u2, space0);\n"
                        "RWTexture2D<float> post_acc : register(u3, space0);\n"
                        "SamplerState post_bilinear : register(s4, space0);\n"
                        "\n"));
    for (auto const& text : {dx12, vulkan})
    {
        CHECK(!text.contains("#pragma"));
        CHECK(!text.contains("namespace"));
    }
}

TEST("sgl emit - each texture shape and depth is its target's own type, and 1D is 2D on WebGPU")
{
    constexpr auto shapes = "binding set:\n"
                            "    a: texture_1d[float]\n"
                            "    b: texture_2d_array[uint4]\n"
                            "    c: texture_cube[float3]\n"
                            "    d: texture_2d_depth\n"
                            "    e: image_3d[.rgba16_float]\n"
                            "    f: comparison_sampler\n"
                            "\n"
                            "@compute(1) fun cs(@thread_id id: int3){set}:\n"
                            "    let unused = id.x\n";
    auto const wgsl = text_of(shapes, target::wgsl);
    CHECK(wgsl.contains("var set_a: texture_2d<f32>;\n"));
    CHECK(wgsl.contains("var set_b: texture_2d_array<u32>;\n"));
    CHECK(wgsl.contains("var set_c: texture_cube<f32>;\n"));
    CHECK(wgsl.contains("var set_d: texture_depth_2d;\n"));
    CHECK(wgsl.contains("var set_e: texture_storage_3d<rgba16float, read>;\n"));
    CHECK(wgsl.contains("var set_f: sampler_comparison;\n"));

    auto const hlsl = text_of(shapes, target::hlsl_dx12);
    CHECK(hlsl.contains("Texture1D<float> set_a : register(t0, space0);\n"));
    CHECK(hlsl.contains("Texture2DArray<uint4> set_b : register(t1, space0);\n"));
    CHECK(hlsl.contains("TextureCube<float3> set_c : register(t2, space0);\n"));
    CHECK(hlsl.contains("Texture2D<float> set_d : register(t3, space0);\n"));
    CHECK(hlsl.contains("RWTexture3D<float4> set_e : register(u4, space0);\n"));
    CHECK(hlsl.contains("SamplerComparisonState set_f : register(s5, space0);\n"));
}

TEST("sgl emit - MSL declines a group of textures as it declines one of buffers")
{
    CHECK(sgl::emit::dump_errors(emit_source(k_blur, 0, target::msl)).contains("unsupported"));
}

TEST("sgl emit - a pixel stage samples with the level its derivatives pick")
{
    constexpr auto lit = "binding material:\n"
                         "    albedo: texture_2d[float3]\n"
                         "    smp: sampler\n"
                         "\n"
                         "struct pixel_input:\n"
                         "    @position position: hpos4\n"
                         "    uv: float2\n"
                         "\n"
                         "@pixel struct target:\n"
                         "    color: float4\n"
                         "\n"
                         "@pixel fun ps(p: pixel_input){material} -> target:\n"
                         "    let c = DEBUG_sample(material.albedo, p.uv, material.smp)\n"
                         "    return {color = float4(c.x, c.y, c.z, 1.0)}\n";
    CHECK(text_of(lit, target::wgsl).contains("let c: vec3f = textureSample(material_albedo, material_smp, p.uv).xyz;\n"));
    CHECK(text_of(lit, target::hlsl_dx12)
              .contains("const float3 c = material_albedo.Sample(material_smp, "
                        "p.uv);\n"));
}

TEST("sgl emit - a size is one HLSL helper per texture type, declared once however often it is called")
{
    constexpr auto sized
        = "binding set:\n"
          "    a: texture_2d[float4]\n"
          "    b: texture_2d[uint]\n"
          "    c: out image_2d[.rgba8_unorm]\n"
          "\n"
          "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
          "    let s = DEBUG_size(set.a, 0) + DEBUG_size(set.a, 1) + DEBUG_size(set.b, 0) + DEBUG_size(set.c)\n"
          "    DEBUG_store(set.c, s, float4(1.0, 1.0, 1.0, 1.0))\n";
    auto const hlsl = text_of(sized, target::hlsl_dx12);
    CHECK(hlsl.contains("int2 sgl_size(Texture2D<float4> t, int level)\n"
                        "{\n"
                        "    uint width, height, levels;\n"
                        "    t.GetDimensions(uint(level), width, height, levels);\n"
                        "    return int2(width, height);\n"
                        "}\n"));
    CHECK(hlsl.contains("int2 sgl_size(Texture2D<uint> t, int level)\n"));
    CHECK(hlsl.contains("int2 sgl_size(RWTexture2D<float4> i)\n"));
    // Two calls on one texture type, one overload.
    auto const first = hlsl.find("int2 sgl_size(Texture2D<float4> t");
    REQUIRE(first >= 0);
    CHECK(!cc::string_view(hlsl)
               .subview({.start = first + 1, .end = hlsl.size()})
               .contains("int2 sgl_size(Texture2D<float4> t"));
    CHECK(hlsl.contains("sgl_size(set_a, 0) + sgl_size(set_a, 1)"));

    auto const wgsl = text_of(sized, target::wgsl);
    CHECK(wgsl.contains("vec2i(textureDimensions(set_a, 0)) + vec2i(textureDimensions(set_a, 1))"));
    CHECK(wgsl.contains("vec2i(textureDimensions(set_c))"));
    CHECK(!wgsl.contains("sgl_size"));
}

TEST("sgl emit - a helper is declared by the entry point that calls it, and by no other of the module")
{
    constexpr auto two = "binding set:\n"
                         "    a: texture_2d[float4]\n"
                         "    c: out image_2d[.rgba8_unorm]\n"
                         "\n"
                         "@compute(8, 8) fun sized(@thread_id id: int3){set}:\n"
                         "    DEBUG_store(set.c, DEBUG_size(set.a, 0), float4(1.0, 1.0, 1.0, 1.0))\n"
                         "\n"
                         "@compute(8, 8) fun plain(@thread_id id: int3){set}:\n"
                         "    DEBUG_store(set.c, int2(id.x, id.y), float4(1.0, 1.0, 1.0, 1.0))\n";
    CHECK(text_of(two, target::hlsl_dx12).contains("int2 sgl_size(Texture2D<float4> t, int level)\n"));

    auto const second = emit_source(two, 1, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(second) == "");
    CHECK(second.text.contains("entry point 'plain'"));
    CHECK(!second.text.contains("sgl_size"));
}

TEST("sgl emit - a local named as a builtin the text calls is renamed, and the call is not")
{
    // WGSL's texture functions are predeclared, so a local of the same name would shadow the call.
    constexpr auto shadowing = "binding set:\n"
                               "    src: texture_2d[float4]\n"
                               "    dst: out image_2d[.rgba8_unorm]\n"
                               "\n"
                               "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                               "    let xy = int2(id.x, id.y)\n"
                               "    let textureLoad = id.z\n"
                               "    DEBUG_store(set.dst, xy, DEBUG_load(set.src, xy, textureLoad))\n";
    auto const wgsl = text_of(shadowing, target::wgsl);
    CHECK(wgsl.contains("    let textureLoad_: i32 = id.z;\n"));
    CHECK(wgsl.contains("    textureStore(set_dst, xy, vec4f(textureLoad(set_src, xy, textureLoad_)));\n"));

    // A texture's load takes its level in the coordinate's third component in HLSL, which reserves no `textureLoad`.
    CHECK(text_of(shadowing, target::hlsl_dx12).contains("    set_dst[xy] = set_src.Load(int3(xy, textureLoad));\n"));

    // `sgl_size` is the helper's name, so HLSL reserves it even where no helper is declared.
    constexpr auto sized = "binding set:\n"
                           "    a: texture_2d[float4]\n"
                           "    c: out image_2d[.rgba8_unorm]\n"
                           "\n"
                           "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                           "    let sgl_size = int2(id.x, id.y)\n"
                           "    DEBUG_store(set.c, DEBUG_size(set.a, 0) + sgl_size, float4(1.0, 1.0, 1.0, 1.0))\n";
    auto const hlsl = text_of(sized, target::hlsl_dx12);
    CHECK(hlsl.contains("    const int2 sgl_size_ = int2(id.x, id.y);\n"));
    CHECK(hlsl.contains("sgl_size(set_a, 0) + sgl_size_"));
}

TEST("sgl emit - an int or a uint image pads a narrow store with zeros of its own kind in WGSL")
{
    constexpr auto integers = "binding set:\n"
                              "    i: out image_2d[.r32_sint]\n"
                              "    u: out image_2d[.r32_uint]\n"
                              "\n"
                              "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                              "    let xy = int2(id.x, id.y)\n"
                              "    DEBUG_store(set.i, xy, id.x)\n"
                              "    DEBUG_store(set.u, xy, id.x as uint)\n";
    auto const wgsl = text_of(integers, target::wgsl);
    CHECK(wgsl.contains("    textureStore(set_i, xy, vec4i(id.x, 0, 0, 0));\n"));
    CHECK(wgsl.contains("    textureStore(set_u, xy, vec4u(u32(id.x), 0u, 0u, 0u));\n"));

    auto const hlsl = text_of(integers, target::hlsl_dx12);
    CHECK(hlsl.contains("RWTexture2D<int> set_i : register(u0, space0);\n"));
    CHECK(hlsl.contains("RWTexture2D<uint> set_u : register(u1, space0);\n"));
    CHECK(hlsl.contains("    set_u[xy] = uint(id.x);\n"));
}

TEST("sgl emit - a granted image format is written like any portable one")
{
    constexpr auto narrow = "require extended_image_formats\n"
                            "\n"
                            "binding set:\n"
                            "    r: out image_2d[.r8_unorm]\n"
                            "\n"
                            "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                            "    DEBUG_store(set.r, int2(id.x, id.y), 0.5)\n";
    CHECK(text_of(narrow, target::wgsl).contains("var set_r: texture_storage_2d<r8unorm, write>;\n"));
    CHECK(text_of(narrow, target::hlsl_vulkan)
              .contains("[[vk::binding(0, 0)]] [[vk::image_format(\"r8\")]] RWTexture2D<float> set_r;\n"));
    CHECK(text_of(narrow, target::hlsl_dx12).contains("RWTexture2D<float> set_r : register(u0, space0);\n"));
}

TEST("sgl emit - WGSL refuses an entry point needing a feature WebGPU never has, by its name")
{
    // EMIT-109: the one refusal by feature that depends on the target; a device of every other target may have it.
    constexpr auto layered = "require multisampled_array_textures\n"
                             "\n"
                             "binding set:\n"
                             "    layers: texture_2d_ms_array[float4]\n"
                             "\n"
                             "@compute(1) fun cs(@thread_id id: int3){set}:\n"
                             "    let unused = id.x\n";
    CHECK(sgl::emit::dump_errors(emit_source(layered, 0, target::wgsl))
          == "target-lacks-feature cs needs multisampled_array_textures, which WebGPU does not have\n");
    CHECK(text_of(layered, target::hlsl_dx12).contains("Texture2DMSArray<float4> set_layers : register(t0, space0);\n"));
    CHECK(text_of(layered, target::hlsl_vulkan).contains("[[vk::binding(0, 0)]] Texture2DMSArray<float4> set_layers;\n"));
}

TEST("sgl emit - a static sampler that compares is a comparison sampler, and its settings stay out of the text")
{
    constexpr auto shadowed = "binding set:\n"
                              "    sampler shadow:\n"
                              "        compare = .less\n"
                              "        max_anisotropy = 16\n"
                              "        min_lod = 0.5\n"
                              "        max_lod = 4.5\n"
                              "        mip_lod_bias = 0.25\n"
                              "\n"
                              "@compute(1) fun cs(@thread_id id: int3){set}:\n"
                              "    let unused = id.x\n";
    // The host's layout carries the state, from `sgl describe`; the shader only declares the sampler.
    CHECK(text_of(shadowed, target::hlsl_vulkan).contains("[[vk::binding(0, 0)]] SamplerComparisonState set_shadow;\n"));
    CHECK(text_of(shadowed, target::hlsl_dx12).contains("SamplerComparisonState set_shadow : register(s0, space0);\n"));
    CHECK(text_of(shadowed, target::wgsl).contains("var set_shadow: sampler_comparison;\n"));
}

TEST("sgl emit - WGSL lets an implicit-derivative sample stand in non-uniform control flow, only where one is called")
{
    // Tint refuses what HLSL accepts, so the directive keeps the program written for every target (EMIT-103).
    constexpr auto branched = "binding material:\n"
                              "    albedo: texture_2d[float4]\n"
                              "    smp: sampler\n"
                              "\n"
                              "struct pixel_input:\n"
                              "    @position position: hpos4\n"
                              "    uv: float2\n"
                              "\n"
                              "@pixel struct target:\n"
                              "    color: float4\n"
                              "\n"
                              "@pixel fun ps(p: pixel_input){material} -> target:\n"
                              "    let mut c = float4(0.0, 0.0, 0.0, 1.0)\n"
                              "    if p.uv.x < 0.5:\n"
                              "        c = DEBUG_sample(material.albedo, p.uv, material.smp)\n"
                              "    return {color = c}\n";
    CHECK(text_of(branched, target::wgsl)
              .contains("// Generated: the SGL source is what to edit.\n"
                        "\n"
                        "diagnostic(off, derivative_uniformity);\n"
                        "\n"
                        "@group(0) @binding(0) var material_albedo: texture_2d<f32>;\n"));
    CHECK(!text_of(branched, target::hlsl_dx12).contains("diagnostic"));

    // A sample at an explicit level takes no derivative, so it leaves Tint's analysis on.
    CHECK(!text_of(k_blur, target::wgsl).contains("diagnostic"));
}
