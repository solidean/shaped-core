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
                                   "    let c = post.src.sample(uv, post.bilinear, level = 0.0)\n"
                                   "    post.dst.store(xy, c)\n"
                                   "    post.acc.store(xy, post.acc.load(xy) + c.x)\n";

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
             "    textureStore(post_dst, xy, c);\n"
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

    // MSL's texture takes the scalar it holds, an image states its access, and a comparison sampler is a sampler.
    auto const msl = text_of(shapes, target::msl);
    CHECK(msl.contains("    texture1d<float> set_a [[id(0)]];\n"));
    CHECK(msl.contains("    texture2d_array<uint> set_b [[id(1)]];\n"));
    CHECK(msl.contains("    texturecube<float> set_c [[id(2)]];\n"));
    CHECK(msl.contains("    depth2d<float> set_d [[id(3)]];\n"));
    CHECK(msl.contains("    texture3d<float, access::read> set_e [[id(4)]];\n"));
    CHECK(msl.contains("    sampler set_f [[id(5)]];\n"));
}

TEST("sgl emit - MSL passes a group of textures as one argument buffer, its static sampler a slot like any other")
{
    // EMIT-89: the slots are the other targets' registers, and the body reads each through a local of its global's name.
    auto const msl = text_of(k_blur, target::msl);
    CHECK(msl.contains("struct post_arguments\n"
                       "{\n"
                       "    constant post_data* post [[id(0)]];\n"
                       "    texture2d<float> post_src [[id(1)]];\n"
                       "    texture2d<float, access::write> post_dst [[id(2)]];\n"
                       "    texture2d<float, access::read_write> post_acc [[id(3)]];\n"
                       "    sampler post_bilinear [[id(4)]];\n"
                       "};\n"));
    CHECK(msl.contains("kernel void blur(uint3 id_in [[thread_position_in_grid]], constant post_arguments& post_group "
                       "[[buffer(0)]])\n"));
    CHECK(msl.contains("    constant auto& post = *post_group.post;\n"));
    CHECK(msl.contains("    constant auto& post_src = post_group.post_src;\n"));
    CHECK(msl.contains("    constant auto& post_bilinear = post_group.post_bilinear;\n"));
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
                         "    let c = material.albedo.sample(p.uv, material.smp)\n"
                         "    return {color = float4(c.x, c.y, c.z, 1.0)}\n";
    CHECK(text_of(lit, target::wgsl).contains("let c: vec3f = textureSample(material_albedo, material_smp, p.uv).xyz;\n"));
    CHECK(text_of(lit, target::hlsl_dx12)
              .contains("const float3 c = material_albedo.Sample(material_smp, "
                        "p.uv);\n"));
}

TEST("sgl emit - a size is one HLSL helper per texture type, declared once however often it is called")
{
    constexpr auto sized = "binding set:\n"
                           "    a: texture_2d[float4]\n"
                           "    b: texture_2d[uint]\n"
                           "    c: out image_2d[.rgba8_unorm]\n"
                           "\n"
                           "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                           "    let s = set.a.size(0) + set.a.size(1) + set.b.size(0) + set.c.size()\n"
                           "    set.c.store(s, float4(1.0, 1.0, 1.0, 1.0))\n";
    auto const hlsl = text_of(sized, target::hlsl_dx12);
    CHECK(hlsl.contains("int2 sgl_size(Texture2D<float4> t, int level)\n"
                        "{\n"
                        "    uint width, height, levels;\n"
                        "    t.GetDimensions(uint(level), width, height, levels);\n"
                        "    return int2(width, height);\n"
                        "}\n"));
    CHECK(hlsl.contains("int2 sgl_size(Texture2D<uint> t, int level)\n"));
    CHECK(hlsl.contains("int2 sgl_size(RWTexture2D<float4> t)\n"));
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
                         "    set.c.store(set.a.size(0), float4(1.0, 1.0, 1.0, 1.0))\n"
                         "\n"
                         "@compute(8, 8) fun plain(@thread_id id: int3){set}:\n"
                         "    set.c.store(int2(id.x, id.y), float4(1.0, 1.0, 1.0, 1.0))\n";
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
                               "    set.dst.store(xy, set.src.load(xy, textureLoad))\n";
    auto const wgsl = text_of(shadowing, target::wgsl);
    CHECK(wgsl.contains("    let textureLoad_: i32 = id.z;\n"));
    CHECK(wgsl.contains("    textureStore(set_dst, xy, textureLoad(set_src, xy, textureLoad_));\n"));

    // A texture's load takes its level in the coordinate's third component in HLSL, which reserves no `textureLoad`.
    CHECK(text_of(shadowing, target::hlsl_dx12).contains("    set_dst[xy] = set_src.Load(int3(xy, textureLoad));\n"));

    // `sgl_size` is the helper's name, so HLSL reserves it even where no helper is declared.
    constexpr auto sized = "binding set:\n"
                           "    a: texture_2d[float4]\n"
                           "    c: out image_2d[.rgba8_unorm]\n"
                           "\n"
                           "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                           "    let sgl_size = int2(id.x, id.y)\n"
                           "    set.c.store(set.a.size(0) + sgl_size, float4(1.0, 1.0, 1.0, 1.0))\n";
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
                              "    set.i.store(xy, id.x)\n"
                              "    set.u.store(xy, id.x as uint)\n";
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
                            "    set.r.store(int2(id.x, id.y), 0.5)\n";
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

TEST("sgl emit - WGSL takes an implicit-derivative sample as it stands, since the check pass judged its uniformity")
{
    // CHK-282 refuses what Tint's analysis would, so no directive switches that analysis off
    constexpr auto sampled = "binding material:\n"
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
                             "    let c = material.albedo.sample(p.uv, material.smp)\n"
                             "    if p.uv.x < 0.5 => return {color = c}\n"
                             "    return {color = float4(0.0, 0.0, 0.0, 1.0)}\n";
    CHECK(!text_of(sampled, target::wgsl).contains("diagnostic"));
}

TEST("sgl emit - a builtin's default fills the level a call leaves out, and a texture stays where it is named")
{
    // The default makes the call bind its arguments first, and a texture is no value a local could hold.
    auto const source = cc::string_view("binding frame:\n"
                                        "    src: texture_2d[float4]\n"
                                        "@compute(8, 8) fun main_cs(@thread_id id: int3){frame}:\n"
                                        "    let c = frame.src.load(int2(id.x, id.y))\n"
                                        "    let s = frame.src.size()\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("textureLoad(frame_src, xy, 0)"));
    CHECK(wgsl.contains("textureDimensions(frame_src, 0)"));
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("frame_src.Load(int3(xy, 0))"));
}

TEST("sgl emit - every texture method is its target's own call, with a layer and an offset where it takes them")
{
    constexpr auto methods
        = "binding set:\n"
          "    @sampler(lin) arr: texture_2d_array[float4]\n"
          "    @sampler(lin) sky: texture_cube[float4]\n"
          "    @sampler(cmp) shadow: texture_2d_depth\n"
          "    line: texture_1d[float]\n"
          "    ms: texture_2d_ms[uint2]\n"
          "    layers: mut image_2d_array[.r32_uint]\n"
          "    lin: sampler\n"
          "    cmp: comparison_sampler\n"
          "\n"
          "struct pixel_input:\n"
          "    @position position: hpos4\n"
          "    uv: float2\n"
          "\n"
          "@pixel struct target:\n"
          "    color: float4\n"
          "\n"
          "@pixel fun ps(p: pixel_input){set} -> target:\n"
          "    let a = set.arr.sample(p.uv, layer = 2)\n"
          "    let b = set.arr.sample(p.uv, layer = 1, bias = 0.5)\n"
          "    let c = set.arr.sample(p.uv, layer = 0, grad_x = p.uv, grad_y = p.uv)\n"
          "    let d = set.arr.gather(p.uv, layer = 1, component = texel_component.z, offset = int2(1, -1))\n"
          "    let e = set.sky.gather(float3(1.0, 0.0, 0.0), component = texel_component.y)\n"
          "    let f = set.shadow.sample_compare(p.uv, reference = 0.5)\n"
          "    let g = set.shadow.sample_compare(p.uv, reference = 0.5, level = 0.0)\n"
          "    let h = set.line.sample(0.5, set.lin, level = 1.0)\n"
          "    let i = set.ms.load(int2(1, 2), sample = 3)\n"
          "    let j = set.layers.load(int2(0, 0), layer = 1)\n"
          "    set.layers.store(int2(0, 0), j, layer = 2)\n"
          "    let k = set.arr.layer_count() + set.ms.sample_count() + set.line.size()\n"
          "    return {color = a + b + c + d + e + float4(f + g + h, (i.x + j) as float, k as float, 1.0)}\n";

    auto const wgsl = text_of(methods, target::wgsl);
    CHECK(wgsl.contains("textureSample(set_arr, set_lin, p.uv, 2)"));
    CHECK(wgsl.contains("textureSampleBias(set_arr, set_lin, p.uv, 1, 0.5)"));
    CHECK(wgsl.contains("textureSampleGrad(set_arr, set_lin, p.uv, 0, p.uv, p.uv)"));
    // the component and the offset are constants WGSL takes only as written
    CHECK(wgsl.contains("textureGather(2, set_arr, set_lin, p.uv, 1, vec2i(1, -1))"));
    CHECK(wgsl.contains("textureGather(1, set_sky, set_lin, vec3f(1.0, 0.0, 0.0))"));
    CHECK(wgsl.contains("textureSampleCompare(set_shadow, set_cmp, p.uv, 0.5)"));
    CHECK(wgsl.contains("textureSampleCompareLevel(set_shadow, set_cmp, p.uv, 0.5)"));
    // a 1D texture is a 2D one on WebGPU, sampled along its middle row
    CHECK(wgsl.contains("textureSampleLevel(set_line, set_lin, vec2f(0.5, 0.5), 1.0).x"));
    CHECK(wgsl.contains("textureLoad(set_ms, vec2i(1, 2), 3).xy"));
    CHECK(wgsl.contains("textureLoad(set_layers, vec2i(0, 0), 1).x"));
    CHECK(wgsl.contains("textureStore(set_layers, vec2i(0, 0), 2, vec4u(j, 0u, 0u, 0u));\n"));
    CHECK(wgsl.contains("i32(textureNumLayers(set_arr)) + i32(textureNumSamples(set_ms)) + "
                        "i32(textureDimensions(set_line, 0).x)"));

    // HLSL packs the layer into the coordinate, and names a gather's component in the method
    auto const hlsl = text_of(methods, target::hlsl_dx12);
    CHECK(hlsl.contains("set_arr.Sample(set_lin, float3(p.uv, float(2)))"));
    CHECK(hlsl.contains("set_arr.SampleBias(set_lin, float3(p.uv, float(1)), 0.5)"));
    CHECK(hlsl.contains("set_arr.SampleGrad(set_lin, float3(p.uv, float(0)), p.uv, p.uv)"));
    CHECK(hlsl.contains("set_arr.GatherBlue(set_lin, float3(p.uv, float(1)), int2(1, -1))"));
    CHECK(hlsl.contains("set_sky.GatherGreen(set_lin, float3(1.0, 0.0, 0.0))"));
    CHECK(hlsl.contains("set_shadow.SampleCmp(set_cmp, p.uv, 0.5)"));
    CHECK(hlsl.contains("set_shadow.SampleCmpLevelZero(set_cmp, p.uv, 0.5)"));
    CHECK(hlsl.contains("set_line.SampleLevel(set_lin, 0.5, 1.0)"));
    CHECK(hlsl.contains("set_ms.Load(int2(1, 2), 3)"));
    CHECK(hlsl.contains("set_layers[int3(int2(0, 0), 1)]"));
    CHECK(hlsl.contains("set_layers[int3(int2(0, 0), 2)] = j;\n"));
    CHECK(hlsl.contains("sgl_layers(set_arr) + sgl_samples(set_ms) + sgl_size(set_line, 0)"));
    CHECK(hlsl.contains("int sgl_samples(Texture2DMS<uint2> t)\n"
                        "{\n"
                        "    uint width, height, samples;\n"
                        "    t.GetDimensions(width, height, samples);\n"
                        "    return int(samples);\n"
                        "}\n"));
}

TEST("sgl emit - a binding array takes consecutive registers, and a marked index is NonUniformResourceIndex")
{
    constexpr auto arrays
        = "require binding_arrays\n"
          "\n"
          "binding materials:\n"
          "    @sampler(smp) albedo: texture_2d[float4][8]\n"
          "    params: buffer[float4][2]\n"
          "    smp: sampler\n"
          "\n"
          "struct pixel_input:\n"
          "    @position position: hpos4\n"
          "    uv: float2\n"
          "    @interpolate(.flat) material: int\n"
          "\n"
          "@pixel struct target:\n"
          "    color: float4\n"
          "\n"
          "@pixel fun ps(p: pixel_input){materials} -> target:\n"
          "    return {color = materials.albedo[nonuniform p.material].sample(p.uv) * materials.params[1][0]}\n";
    auto const dx12 = text_of(arrays, target::hlsl_dx12);
    CHECK(dx12.contains("Texture2D<float4> materials_albedo[8] : register(t0, space0);\n"
                        "StructuredBuffer<float4> materials_params[2] : register(t8, space0);\n"
                        "SamplerState materials_smp : register(s10, space0);\n"));
    CHECK(dx12.contains("materials_albedo[NonUniformResourceIndex(p.material)].Sample(materials_smp, p.uv) * "
                        "materials_params[1][0]"));
    CHECK(text_of(arrays, target::hlsl_vulkan)
              .contains("[[vk::binding(8, 0)]] StructuredBuffer<float4> materials_params[2];\n"));

    // WebGPU has no binding arrays, so WGSL refuses by the feature rather than guessing
    CHECK(sgl::emit::dump_errors(emit_source(arrays, 0, target::wgsl)).contains("binding_arrays"));
}

TEST("sgl emit - a `for` end that takes derivatives is evaluated once, ahead of the loop")
{
    // LEGAL-7: re-tested after a divergent `break`, the sample would run in part of a quad
    constexpr auto source = "binding material:\n"
                            "    @sampler(smp) albedo: texture_2d[float4]\n"
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
                            "    for i in 0 ..< ((material.albedo.sample(float2(0.5, 0.5)).x * 4.0) as int):\n"
                            "        if p.uv.x < 0.5 => break\n"
                            "        c.x += 1.0\n"
                            "    return {color = c}\n";
    CHECK(text_of(source, target::wgsl)
              .contains("    let i_end: i32 = i32(textureSample(material_albedo, material_smp, vec2f(0.5, 0.5)).x * "
                        "4.0);\n"
                        "    for (var i: i32 = 0; i < i_end; i++) {\n"));
    CHECK(text_of(source, target::hlsl_dx12).contains("i < i_end; "));
}

TEST("sgl emit - a marked index keeps its mark when a later argument moves it into a local")
{
    // `pick` returns early, so its value is computed ahead of the call, and the index ahead of that
    constexpr auto pinned = "require binding_arrays\n"
                            "\n"
                            "binding mats:\n"
                            "    texs: texture_2d[float4][4]\n"
                            "\n"
                            "binding results:\n"
                            "    values: mut buffer[float4]\n"
                            "\n"
                            "fun pick(x: int) -> int:\n"
                            "    if x == 0 => return 1\n"
                            "    return 0\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){mats, results}:\n"
                            "    results.values[id.x] = mats.texs[nonuniform (id.x % 4)].load(int2(pick(id.x), 0))\n";
    auto const dx12 = text_of(pinned, target::hlsl_dx12);
    CHECK(dx12.contains("const int index = id.x % 4;\n"));
    CHECK(dx12.contains("mats_texs[NonUniformResourceIndex(index)].Load("));
}

TEST("sgl emit - a depth texture's level is an int, which WGSL takes whole and the others convert")
{
    constexpr auto depth = "binding set:\n"
                           "    @sampler(pt) shadow: texture_2d_depth\n"
                           "    @non_filtering pt: sampler\n"
                           "\n"
                           "struct pixel_input:\n"
                           "    @position position: hpos4\n"
                           "    uv: float2\n"
                           "    @interpolate(.flat) lod: int\n"
                           "\n"
                           "@pixel struct target:\n"
                           "    color: float4\n"
                           "\n"
                           "@pixel fun ps(p: pixel_input){set} -> target:\n"
                           "    let d = set.shadow.sample(p.uv, level = p.lod)\n"
                           "    return {color = float4(d, d, d, 1.0)}\n";
    CHECK(text_of(depth, target::wgsl).contains("textureSampleLevel(set_shadow, set_pt, p.uv, p.lod)"));
    CHECK(text_of(depth, target::hlsl_dx12).contains("set_shadow.SampleLevel(set_pt, p.uv, float(p.lod))"));
}

namespace
{
/// Three file-scope samplers, of which the first entry point reaches the second and third, one through a helper.
constexpr cc::string_view k_file_samplers
    = "sampler unused:\n"
      "    filter = .linear\n"
      "\n"
      "sampler edge:\n"
      "    filter = .nearest\n"
      "    address = .clamp_edge\n"
      "\n"
      "sampler shadow:\n"
      "    compare = .less\n"
      "\n"
      "binding set:\n"
      "    src: texture_2d[float4]\n"
      "    depth: texture_2d_depth\n"
      "    dst: out image_2d[.rgba8_unorm]\n"
      "\n"
      "fun lit(uv: float2){set} -> float:\n"
      "    return set.depth.sample_compare(uv, shadow, reference = 0.5, level = 0.0)\n"
      "\n"
      "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
      "    let xy = int2(id.x, id.y)\n"
      "    let uv = float2(-0.5, 1.5)\n"
      "    set.dst.store(xy, set.src.sample(uv, edge, level = 0.0) * lit(uv))\n"
      "\n"
      "@compute(8, 8) fun plain(@thread_id id: int3){set}:\n"
      "    set.dst.store(int2(id.x, id.y), float4(1.0, 1.0, 1.0, 1.0))\n";
} // namespace

TEST("sgl emit - a file-scope sampler is at its declaration's index among the file's, outside every group")
{
    CHECK(text_of(k_file_samplers, target::hlsl_dx12)
          == "// SGL compute entry point 'cs', written as HLSL for dx12.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "Texture2D<float4> set_src : register(t0, space0);\n"
             "Texture2D<float> set_depth : register(t1, space0);\n"
             "RWTexture2D<float4> set_dst : register(u2, space0);\n"
             "\n"
             "SamplerState edge : register(s1, space10);\n"
             "SamplerComparisonState shadow : register(s2, space10);\n"
             "\n"
             "[numthreads(8, 8, 1)]\n"
             "void cs(uint3 id_in : SV_DispatchThreadID)\n"
             "{\n"
             "    const int3 id = int3(id_in);\n"
             "    const int2 xy = int2(id.x, id.y);\n"
             "    const float2 uv = float2(-0.5, 1.5);\n"
             "    set_dst[xy] = set_src.SampleLevel(edge, uv, 0.0) * set_depth.SampleCmpLevelZero(shadow, uv, 0.5);\n"
             "}\n");

    // WebGPU keeps binding 0 of sg's own group for the inline constants, so index i is binding i + 1
    // vulkan's inline constants are push constants, and it takes the same i + 1 for parity
    CHECK(text_of(k_file_samplers, target::hlsl_vulkan)
              .contains("[[vk::binding(2, 0)]] [[vk::image_format(\"rgba8\")]] RWTexture2D<float4> set_dst;\n"
                        "\n"
                        "[[vk::binding(2, 3)]] SamplerState edge;\n"
                        "[[vk::binding(3, 3)]] SamplerComparisonState shadow;\n"
                        "\n"));
    CHECK(text_of(k_file_samplers, target::wgsl)
              .contains("@group(0) @binding(2) var set_dst: texture_storage_2d<rgba8unorm, write>;\n"
                        "\n"
                        "@group(3) @binding(2) var edge: sampler;\n"
                        "@group(3) @binding(3) var shadow: sampler_comparison;\n"
                        "\n"));
}

TEST("sgl emit - an entry point declares only the file-scope samplers its code reaches, and binds each by its name")
{
    auto const cs = emit_source(k_file_samplers, 0, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(cs) == "");
    CHECK(!cs.text.contains("unused"));
    auto hosts = cc::string();
    for (auto const& b : cs.bound_names)
        hosts.appendf("{}={} ", b.host, b.emitted);
    CHECK(hosts == "set.src=set_src set.depth=set_depth set.dst=set_dst edge=edge shadow=shadow ");

    auto const plain = emit_source(k_file_samplers, 1, target::wgsl);
    CHECK(sgl::emit::dump_errors(plain) == "");
    CHECK(!plain.text.contains("sampler"));
    CHECK(!plain.text.contains("@group(3)"));
}

TEST("sgl emit - a texture's @sampler naming a file-scope sampler reaches it, so the entry point declares it")
{
    constexpr auto source = "sampler unused:\n"
                            "    filter = .linear\n"
                            "\n"
                            "sampler edge:\n"
                            "    filter = .nearest\n"
                            "\n"
                            "binding set:\n"
                            "    @sampler(edge) src: texture_2d[float4]\n"
                            "    dst: out image_2d[.rgba8_unorm]\n"
                            "\n"
                            "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
                            "    set.dst.store(int2(id.x, id.y), set.src.sample(float2(0.5, 0.5), level = 0.0))\n";
    auto const dx12 = emit_source(source, 0, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(dx12) == "");
    CHECK(dx12.text.contains("SamplerState edge : register(s1, space10);\n"));
    CHECK(dx12.text.contains("set_src.SampleLevel(edge, float2(0.5, 0.5), 0.0)"));
    CHECK(!dx12.text.contains("unused"));
    CHECK((dx12.bound_names.back() == sgl::emit::bound_name{.emitted = "edge", .host = "edge"}));
    CHECK(text_of(source, target::wgsl).contains("@group(3) @binding(2) var edge: sampler;\n"));
}

TEST("sgl emit - a file-scope sampler reached at index 16 or more is too-many-samplers on every target")
{
    // seventeen samplers, of which only the last is reached: the sixteen above it still count toward its index
    auto samplers = cc::string();
    for (auto i = 0; i < 17; ++i)
        samplers.appendf("sampler s{}:\n    filter = .linear\n\n", i);
    auto const source_using = [&](cc::string_view name)
    {
        return cc::format("{}binding set:\n"
                          "    src: texture_2d[float4]\n"
                          "    dst: out image_2d[.rgba8_unorm]\n"
                          "\n"
                          "@compute(8, 8) fun cs(@thread_id id: int3){{set}}:\n"
                          "    set.dst.store(int2(id.x, id.y), set.src.sample(float2(0.5, 0.5), {}, level = 0.0))\n",
                          samplers, name);
    };
    for (auto const t : sgl::emit::all_targets())
    {
        CHECK(sgl::emit::dump_errors(emit_source(source_using("s16"), 0, t))
                  .contains("too-many-samplers 'cs' reaches sampler 's16' at index 16, and a stage holds 16; every "
                            "sampler declared above it counts toward its index, whether reached or not"));
        CHECK(!sgl::emit::dump_errors(emit_source(source_using("s15"), 0, t)).contains("too-many-samplers"));
    }
}

TEST("sgl emit - a file-scope sampler named as a word the target reserves is renamed there, and bound by its own name")
{
    constexpr auto reserved
        = "sampler register:\n"
          "    filter = .nearest\n"
          "\n"
          "binding set:\n"
          "    src: texture_2d[float4]\n"
          "    dst: out image_2d[.rgba8_unorm]\n"
          "\n"
          "@compute(8, 8) fun cs(@thread_id id: int3){set}:\n"
          "    set.dst.store(int2(id.x, id.y), set.src.sample(float2(0.5, 0.5), register, level = 0.0))\n";
    auto const dx12 = emit_source(reserved, 0, target::hlsl_dx12);
    CHECK(sgl::emit::dump_errors(dx12) == "");
    CHECK(dx12.text.contains("SamplerState register_ : register(s0, space10);\n"));
    CHECK((dx12.bound_names.back() == sgl::emit::bound_name{.emitted = "register_", .host = "register"}));
}
